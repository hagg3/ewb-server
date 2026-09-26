// durable_write_test.cpp — offline checks for durable_write.h (ROADMAP-SERVER
// stage 7.29): the replace-a-file helper the world, player, sidecar and import
// writers share.
//
//   c++ -std=c++17 -O2 -Wall durable_write_test.cpp -o durable_write_test
//   ./durable_write_test
//
// A test can prove the observable contract — contents replaced whole, the temp
// file never left behind, failure leaves the old file untouched — but not that
// the bytes reached stable storage; that is what the fsync calls are for, and
// only a power cut checks it.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "durable_write.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                          \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

static bool read_all(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

static void test_dirname() {
    CHECK(ewb::durable_dirname("a.txt") == ".", "bare name -> .");
    CHECK(ewb::durable_dirname("/a.txt") == "/", "root file -> /");
    CHECK(ewb::durable_dirname("worlds/x/eden_world.model") == "worlds/x", "nested path");
}

static void test_write_and_replace(const std::string& dir) {
    const std::string path = dir + "/w.model";
    std::string err, got;
    uint64_t us = 12345;

    CHECK(ewb::write_file_durable(path, std::string("first"), err, &us), "create succeeds");
    CHECK(read_all(path, got) && got == "first", "created contents");
    CHECK(us != 12345, "sync time is reported");
    CHECK(!exists(path + ".tmp"), "no temp file left after create");

    CHECK(ewb::write_file_durable(path, std::string("second, longer"), err), "replace succeeds");
    CHECK(read_all(path, got) && got == "second, longer", "replaced whole");

    // A shorter replacement must not leave the tail of the longer one (O_TRUNC).
    CHECK(ewb::write_file_durable(path, std::string("s"), err), "shrink succeeds");
    CHECK(read_all(path, got) && got == "s", "shrunk without stale tail");

    CHECK(ewb::write_file_durable(path, std::string(), err), "empty body succeeds");
    CHECK(read_all(path, got) && got.empty(), "empty body written");
    CHECK(!exists(path + ".tmp"), "no temp file left");
}

static void test_binary_and_large(const std::string& dir) {
    const std::string path = dir + "/big.bin";
    std::string body(5u << 20, '\0');
    for (size_t i = 0; i < body.size(); ++i) body[i] = char((i * 131 + 7) & 0xFF);   // NULs included
    std::string err, got;
    CHECK(ewb::write_file_durable(path, body, err), "5 MB binary write succeeds");
    CHECK(read_all(path, got) && got == body, "5 MB binary round-trips byte for byte");
}

static void test_failure_keeps_old(const std::string& dir) {
    const std::string path = dir + "/keep.model";
    std::string err, got;
    CHECK(ewb::write_file_durable(path, std::string("original"), err), "seed file");

    // Directory missing: the temp file cannot be created; the error names it.
    const std::string bad = dir + "/no/such/dir/x.model";
    err.clear();
    CHECK(!ewb::write_file_durable(bad, std::string("x"), err), "missing directory fails");
    CHECK(err.find("cannot open") != std::string::npos, "error says what failed");

    // Destination is a non-empty directory: the write and fsync succeed but the
    // rename cannot. The temp file must be cleaned up and the error reported.
    const std::string blocked = dir + "/blocked";
    ::mkdir(blocked.c_str(), 0755);
    std::ofstream(blocked + "/child") << "x";
    err.clear();
    CHECK(!ewb::write_file_durable(blocked, std::string("x"), err), "rename onto a directory fails");
    CHECK(err.find("rename") != std::string::npos, "rename failure is named");
    CHECK(!exists(blocked + ".tmp"), "temp file removed after a failed rename");

    CHECK(read_all(path, got) && got == "original", "unrelated file untouched");
}

static void test_fsync_dir(const std::string& dir) {
    std::string err;
    CHECK(ewb::durable_fsync_dir(dir, err), "fsync of a real directory succeeds");
    err.clear();
    CHECK(!ewb::durable_fsync_dir(dir + "/nope", err), "fsync of a missing directory fails");
    CHECK(!err.empty(), "and says why");
}

// Stage 8.6: the mode is set exactly — not narrowed by the umask, not inherited
// from an old file or a stale temp.
static void test_mode(const std::string& dir) {
    const std::string p = dir + "/secret.txt";
    std::string err;
    struct stat st;
    const mode_t old = ::umask(0077);
    CHECK(ewb::write_file_durable(p, std::string("a"), err), "default-mode write");
    CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644, "default is 0644 despite umask 077");
    ::umask(0);
    CHECK(ewb::write_file_durable(p, std::string("b"), err, nullptr, 0600), "0600 write");
    CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "0600 replaces a 0644 file as 0600");
    // A stale world-readable temp file must not leak its mode into the result.
    { std::ofstream t(p + ".tmp"); t << "stale"; }
    ::chmod((p + ".tmp").c_str(), 0666);
    CHECK(ewb::write_file_durable(p, std::string("c"), err, nullptr, 0600), "0600 write over a stale temp");
    CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "a stale 0666 temp does not survive");
    ::umask(old);
}

int main() {
    char tmpl[] = "/tmp/durable_write_test.XXXXXX";
    if (!::mkdtemp(tmpl)) { std::perror("mkdtemp"); return 2; }
    const std::string dir = tmpl;

    test_dirname();
    test_write_and_replace(dir);
    test_binary_and_large(dir);
    test_failure_keeps_old(dir);
    test_fsync_dir(dir);
    test_mode(dir);

    const std::string cmd = "rm -rf '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", dir.c_str());

    if (g_fail) {
        std::fprintf(stderr, "durable_write_test: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("durable_write_test: all checks passed\n");
    return 0;
}
