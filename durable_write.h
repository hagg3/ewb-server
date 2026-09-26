// durable_write.h — write a file so that either the old or the new contents
// survive a power loss, never a zero-length or partial one.
//
// ROADMAP-SERVER stage 7.29. Every writer here used `ofstream` -> `flush()` ->
// `rename()`. `flush()` only hands the bytes to the kernel; it does not put them
// on stable storage, and the directory entry the `rename` creates is not durable
// either. After a power loss or hard VM reset the rename can be on disk while the
// data is not, leaving an empty `eden_world.model` where a whole one used to be.
// ext4's `auto_da_alloc` covers the common case; XFS, btrfs and a VPS with an
// aggressive write cache promise nothing of the sort.
//
// The sequence that is actually durable:
//
//     write temp  ->  fsync(temp)  ->  rename(temp, path)  ->  fsync(dir)
//
// POSIX only (the server is a POSIX TU); header-only so eden_import, eden_export
// and the server share one implementation and one offline suite
// (`durable_write_test.cpp`).

#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ewb {

/// fsync that means it. On macOS plain `fsync` only reaches the drive's cache;
/// `F_FULLFSYNC` is the call that asks the drive to flush. Falls back to `fsync`
/// where the filesystem does not support it (some network mounts).
inline int durable_fsync(int fd) {
#if defined(F_FULLFSYNC)
    if (::fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    return ::fsync(fd);
}

/// The directory part of `path` ("." for a bare filename, "/" for a root file).
inline std::string durable_dirname(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

/// fsync a directory so a `rename()` into it survives a crash. Best effort in the
/// sense that some filesystems refuse `fsync` on a directory fd (EINVAL); that is
/// treated as "nothing more can be done", not as a failed write.
inline bool durable_fsync_dir(const std::string& dir, std::string& err) {
    int fd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                     | O_DIRECTORY
#endif
    );
    if (fd < 0) { err = "cannot open directory " + dir + ": " + std::strerror(errno); return false; }
    const int rc = durable_fsync(fd);
    const int e = errno;
    ::close(fd);
    if (rc != 0 && e != EINVAL && e != ENOTSUP) {
        err = "fsync of directory " + dir + " failed: " + std::strerror(e);
        return false;
    }
    return true;
}

/// Atomically and durably replace `path` with `data[0..len)`.
///
/// Writes `path + ".tmp"`, fsyncs it, renames it over `path`, then fsyncs the
/// containing directory. On any failure the temp file is removed, `path` is left
/// as it was, and `err` says what failed. `sync_micros`, if given, receives the
/// time spent in the two fsyncs — the cost the roadmap wanted measured on the
/// autosave path.
///
/// Callers that need mutual exclusion (the server's `g_saveMtx`) provide it; the
/// fixed temp name means two concurrent writers to one path would collide.
///
/// `mode` is the new file's permission bits, set exactly (not filtered by the
/// umask, and not inherited from a stale temp file): 0644 for world data, 0600
/// for anything secret-adjacent such as `eden_auth.txt` (stage 8.6).
inline bool write_file_durable(const std::string& path, const char* data, size_t len,
                               std::string& err, uint64_t* sync_micros = nullptr,
                               mode_t mode = 0644) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { err = "cannot open " + tmp + ": " + std::strerror(errno); return false; }
    if (::fchmod(fd, mode) != 0) {
        err = "cannot set the mode of " + tmp + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }

    auto fail = [&](const std::string& what) {
        err = what + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    };

    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return fail("write error on " + tmp);
        }
        off += size_t(n);
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (durable_fsync(fd) != 0) return fail("fsync of " + tmp + " failed");
    if (::close(fd) != 0) {
        err = "close of " + tmp + " failed: " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename " + tmp + " -> " + path + " failed: " + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    // The data is durable and the name now points at it; make the name durable
    // too. A failure here is reported, but the file is already in place.
    const bool dir_ok = durable_fsync_dir(durable_dirname(path), err);
    if (sync_micros)
        *sync_micros = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - t0).count();
    return dir_ok;
}

inline bool write_file_durable(const std::string& path, const std::string& body,
                               std::string& err, uint64_t* sync_micros = nullptr,
                               mode_t mode = 0644) {
    return write_file_durable(path, body.data(), body.size(), err, sync_micros, mode);
}

}  // namespace ewb
