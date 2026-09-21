// eden_export.cpp — convert a hosted server world back into a `.eden` file.
//
// ROADMAP-SERVER stage 5.5 (Phase 5). Offline tool, the inverse of
// `eden_import`; it never talks to a running server and never touches the
// network. Run it against a *stopped* world directory (or a copy — a backup
// unpacked on the laptop is the intended input).
//
//   ./eden_export <world-dir> --out <file.eden> [options]
//
// Reads `<world-dir>/eden_world.model` (EDMB or the legacy text form), plus
// `eden_signs.txt`, `eden_spawn.txt` and `eden_origin.txt` (the source header's
// seed / yaw / home / sky, written by `eden_import`, stage 5.6) if they are
// there, and writes the
// `.eden` — with the signs as a `signs_<file>.dat` sidecar beside it by default,
// which is the name both the game and `eden_import --signs` look for. It never
// reads or writes `eden_players.txt`: player positions are not world data and
// have no business leaving the server.
//
// This file owns argument parsing, file I/O, the atomic temp+rename write and
// the summary. Every conversion decision lives in `eden_export.h`, which is pure
// and unit-tested (`eden_export_test.cpp`). See `docs/export.md`.
//
//   clang++ -std=c++17 -O2 -Wall eden_export.cpp -lz -o eden_export

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "eden_export.h"

using namespace ewb;

// ── small file helpers (the same discipline as eden_import.cpp) ─────────────

static bool read_file(const std::string& path, std::string& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path + ": " + std::strerror(errno); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (f.bad()) { err = "read error on " + path; return false; }
    return true;
}

static bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

/// Write `body` to `path.tmp`. The rename is a separate step so several files
/// can be staged and then published together (or all discarded).
static bool stage_file(const std::string& path, const void* data, size_t len, std::string& err) {
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) { err = "cannot open " + tmp + ": " + std::strerror(errno); return false; }
    if (len) f.write(reinterpret_cast<const char*>(data), std::streamsize(len));
    f.flush();
    if (!f) {
        err = "write error on " + tmp;
        f.close();
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

static bool publish_file(const std::string& path, std::string& err) {
    const std::string tmp = path + ".tmp";
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename " + tmp + " -> " + path + " failed: " + std::strerror(errno);
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

static void discard_file(const std::string& path) {
    const std::string tmp = path + ".tmp";
    ::remove(tmp.c_str());
}

static std::string commas(size_t v) {
    std::string s = std::to_string(v), out;
    int n = 0;
    for (size_t i = s.size(); i-- > 0;) {
        out += s[i];
        if (++n % 3 == 0 && i) out += ',';
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/// Last path component, with any trailing slashes and a `.eden` suffix removed —
/// the default world name for `worlds/carved/` or `carved/`.
static std::string basename_of(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

// ── usage ───────────────────────────────────────────────────────────────────

static void usage() {
    std::cout <<
"eden_export — convert a hosted ewb-server world back into a .eden file.\n"
"\n"
"  eden_export <world-dir> --out <file.eden> [options]\n"
"\n"
"<world-dir> holds eden_world.model (EDMB or the legacy text form) and, if they\n"
"exist, eden_signs.txt, eden_spawn.txt and eden_origin.txt. eden_players.txt is\n"
"never read.\n"
"Export the world while the server is stopped, or export a copy.\n"
"\n"
"Output\n"
"  --out FILE               the .eden to write (required)\n"
"  --name NAME              world name in the header (default: the directory's name)\n"
"  --force                  overwrite an existing output file\n"
"  --dry-run                project and print the summary, write nothing\n"
"  --zip                    wrap the file in a ZIP container (the editor reads both)\n"
"\n"
"Terrain\n"
"  --base-profile default|none|FILE   terrain written into untouched columns.\n"
"                           Must match the profile the world was imported with,\n"
"                           or the round trip re-terraforms it. Default: default.\n"
"  --z auto|64|256          height format (default: auto — 64z unless something\n"
"                           sits above height 63). Anything above the chosen\n"
"                           ceiling is dropped and counted, never wrapped.\n"
"\n"
"Signs\n"
"  --signs sidecar|inline|none   where signs go (default: sidecar —\n"
"                           signs_<file>.dat beside the .eden, the name the game\n"
"                           and `eden_import --signs` both look for)\n"
"  --sidecar FILE           write the sidecar here instead\n"
"\n"
"Header fields the server does not store (see docs/export.md § The origin sidecar)\n"
"  --origin FILE            replay this eden_origin.txt (default: <world-dir>/eden_origin.txt\n"
"                           if it exists — written by eden_import)\n"
"  --no-origin              ignore any eden_origin.txt; export with plain defaults\n"
"  --seed N                 world seed (default: the origin's, else 0)\n"
"  --yaw F                  spawn facing (default: the origin's, else 0)\n"
"\n"
"Limits\n"
"  --max-bytes N            refuse to write a file larger than this\n"
"                           (default: 536870912, the parser's own inflate cap)\n"
"\n"
"  -h, --help               this text\n"
"\n"
"Summary goes to stdout, warnings and errors to stderr. Exit 0 on success,\n"
"1 on a refusal or an I/O failure (no file is written), 2 on a usage error.\n"
"See docs/export.md.\n";
}

// ── main ────────────────────────────────────────────────────────────────────

namespace {

bool parse_size(const std::string& s, size_t& out) {
    if (s.empty()) return false;
    char* endp = nullptr;
    const long long v = std::strtoll(s.c_str(), &endp, 10);
    if (endp != s.c_str() + s.size() || v < 0) return false;
    out = size_t(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir, out_path, sidecar_path, origin_path, base_profile = "default", z_arg = "auto";
    ExportOptions opt;
    OriginPins pins;
    bool force = false, dry_run = false, help = false, no_origin = false;

    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "eden_export: " << f << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if      (f == "-h" || f == "--help") help = true;
        else if (f == "--out")            out_path = next("a file");
        else if (f == "--name")         { opt.name = next("a name"); pins.name = true; }
        else if (f == "--force")          force = true;
        else if (f == "--dry-run")        dry_run = true;
        else if (f == "--zip")            opt.zip = true;
        else if (f == "--base-profile") { base_profile = next("default|none|FILE"); pins.profile = true; }
        else if (f == "--origin")         origin_path = next("a file");
        else if (f == "--no-origin")      no_origin = true;
        else if (f == "--z")              z_arg = next("auto|64|256");
        else if (f == "--sidecar")        sidecar_path = next("a file");
        else if (f == "--signs") {
            const std::string v = next("sidecar|inline|none");
            if (!eden_parse_sign_mode(v, opt.signs)) {
                std::cerr << "eden_export: unknown --signs " << v << " (sidecar|inline|none)\n";
                return 2;
            }
        }
        else if (f == "--seed")         { opt.seed = int32_t(std::strtol(next("a seed").c_str(), nullptr, 10)); pins.seed = true; }
        else if (f == "--yaw")          { opt.yaw  = float(std::atof(next("an angle").c_str())); pins.yaw = true; }
        else if (f == "--max-bytes") {
            if (!parse_size(next("a byte count"), opt.max_bytes) || opt.max_bytes == 0) {
                std::cerr << "eden_export: --max-bytes needs a positive integer\n";
                return 2;
            }
        }
        else if (!f.empty() && f[0] == '-') {
            std::cerr << "eden_export: unknown option " << f << " (try --help)\n";
            return 2;
        }
        else if (dir.empty())             dir = f;
        else {
            std::cerr << "eden_export: unexpected argument " << f << "\n";
            return 2;
        }
    }

    if (help || dir.empty()) {
        usage();
        return help ? 0 : 2;
    }
    if (out_path.empty()) {
        std::cerr << "eden_export: --out <file.eden> is required (try --help)\n";
        return 2;
    }
    if (!eden_parse_z_format(z_arg, opt.force_z)) {
        std::cerr << "eden_export: --z wants auto, 64 or 256\n";
        return 2;
    }

    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (no_origin && !origin_path.empty()) {
        std::cerr << "eden_export: --origin and --no-origin contradict each other\n";
        return 2;
    }

    std::string err;

    // ── the origin sidecar (stage 5.6) ──────────────────────────────────────
    // Read now, applied after the spawn file is known (the live `eden_spawn.txt`
    // outranks the origin's snapshot of the header position). An explicit
    // `--origin` must exist; the default one is optional. Either way a file that
    // is *there* and malformed is a refusal — replaying half a header quietly
    // would be worse than stopping.
    EdenOrigin origin;
    std::string origin_source;
    if (!no_origin) {
        const std::string cand = origin_path.empty() ? dir + "/" + EDEN_ORIGIN_FILE : origin_path;
        if (!origin_path.empty() || path_exists(cand)) {
            std::string text;
            if (!read_file(cand, text, err)) {
                std::cerr << "eden_export: " << err << "\n";
                return 1;
            }
            size_t unknown = 0;
            if (!eden_parse_origin(text, origin, err, &unknown)) {
                std::cerr << "eden_export: " << cand << ": " << err << "\n";
                return 1;
            }
            if (unknown)
                std::cerr << "eden_export: warning: " << unknown << " unrecognised key(s) in "
                          << cand << ", ignored\n";
            origin_source = cand;
        }
    }

    // ── the base profile ────────────────────────────────────────────────────
    // Only when named on the command line; otherwise the default stands until the
    // origin (applied below) has had its say.
    if (!pins.profile || base_profile == "default") opt.profile = eden_default_profile();
    else if (base_profile == "none") opt.profile = eden_empty_profile();
    else {
        std::string text;
        if (!read_file(base_profile, text, err)) {
            std::cerr << "eden_export: " << err << "\n";
            return 1;
        }
        if (!eden_parse_profile(text, opt.profile, err)) {
            std::cerr << "eden_export: " << base_profile << ": " << err << "\n";
            return 1;
        }
    }

    // ── the world ───────────────────────────────────────────────────────────
    const std::string model_path = dir + "/eden_world.model";
    std::string blob;
    if (!read_file(model_path, blob, err)) {
        std::cerr << "eden_export: " << err << "\n"
                  << "eden_export: <world-dir> must be a directory holding eden_world.model\n";
        return 1;
    }
    WorldStore store;
    std::string world_format;
    if (WorldStore::is_edmb(blob.data(), blob.size())) {
        if (!store.load_edmb(blob.data(), blob.size(), err)) {
            std::cerr << "eden_export: " << model_path << ": " << err << "\n";
            return 1;
        }
        world_format = "EDMB";
    } else {
        std::istringstream ss(blob);
        world_load_text(ss, store);
        world_format = "legacy text";
    }
    blob.clear();
    blob.shrink_to_fit();

    // ── signs ───────────────────────────────────────────────────────────────
    std::vector<Sign> signs;
    size_t sign_lines_bad = 0;
    const std::string signs_path = dir + "/eden_signs.txt";
    if (path_exists(signs_path)) {
        std::string text;
        if (!read_file(signs_path, text, err)) {
            std::cerr << "eden_export: " << err << "\n";
            return 1;
        }
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            Sign s;
            bool skip = false;
            if (parse_sign_line(line, s, skip)) signs.push_back(std::move(s));
            else if (!skip) ++sign_lines_bad;
        }
    }
    if (sign_lines_bad)
        std::cerr << "eden_export: warning: " << sign_lines_bad << " unparseable line(s) in "
                  << signs_path << ", skipped\n";

    // ── spawn ───────────────────────────────────────────────────────────────
    const std::string spawn_path = dir + "/eden_spawn.txt";
    std::string spawn_source = "default (plane centre)";
    if (path_exists(spawn_path)) {
        std::string text;
        if (!read_file(spawn_path, text, err)) {
            std::cerr << "eden_export: " << err << "\n";
            return 1;
        }
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            Spawn s;
            bool skip = false;
            if (parse_spawn_line(line, s, skip)) {
                opt.spawn = s;
                opt.have_spawn = true;
                spawn_source = "eden_spawn.txt";
                break;
            }
            if (!skip) {
                std::cerr << "eden_export: warning: unparseable line in " << spawn_path
                          << ", ignored\n";
                break;
            }
        }
    }

    // ── replay the origin ───────────────────────────────────────────────────
    // A live `eden_spawn.txt` pins the header position; explicit flags pin their
    // own fields. Everything else the sidecar says is replayed.
    pins.spawn = opt.have_spawn;
    OriginReplay replay;
    if (!origin_source.empty()) replay = eden_apply_origin(origin, pins, opt);
    if (replay.custom_profile_unpinned)
        std::cerr << "eden_export: warning: " << origin_source << " says the world was imported"
                     " with a custom --base-profile FILE. Pass the same --base-profile FILE"
                     " now, or untouched columns are re-terraformed with the default profile.\n";
    for (const std::string& k : replay.keys)
        if (k == "pos") spawn_source = std::string(EDEN_ORIGIN_FILE) + (pins.spawn ? " (spawn file unedited)" : "");
    if (opt.name.empty()) opt.name = basename_of(dir);

    // ── build ───────────────────────────────────────────────────────────────
    ExportResult r;
    if (!eden_export_world(store, signs, opt, r, err)) {
        std::cerr << "eden_export: error: " << err << "\n";
        return 1;
    }

    const std::string sidecar =
        sidecar_path.empty() ? eden_sidecar_path(out_path) : sidecar_path;
    const bool want_sidecar = opt.signs == SignMode::Sidecar && !r.sign_sidecar.empty();

    // ── summary (stdout; one grep-able `key: value` per line) ───────────────
    std::cout << "world: " << (opt.name.empty() ? "(unnamed)" : opt.name) << "\n"
              << "source: " << dir << " (" << world_format << ", "
              << commas(store.size()) << " cells)\n"
              << "z-format: " << r.z_format() << "\n"
              << "chunks: " << r.chunks << (r.seeded ? "  (empty world: one base chunk)" : "")
              << "\n"
              << "cells: " << r.cells << "\n"
              << "cells-dropped-height: " << r.cells_dropped_height << "\n"
              << "cells-dropped-coord: " << r.cells_dropped_coord << "\n"
              << "signs: " << r.signs << "\n"
              << "signs-dropped: " << r.signs_dropped << "\n"
              << "signs-mode: " << eden_sign_mode_name(opt.signs) << "\n";
    if (want_sidecar) std::cout << "signs-file: " << sidecar << "\n";
    {
        char b[96];
        std::snprintf(b, sizeof b, "%.2f:%.2f:%.2f", opt.spawn.x, opt.spawn.y, opt.spawn.z);
        std::cout << "spawn: " << b << " (" << spawn_source << ")\n";
    }
    if (!origin_source.empty()) {
        std::string keys;
        for (const std::string& k : replay.keys) keys += (keys.empty() ? "" : ",") + k;
        std::cout << "origin: " << origin_source << " (" << (keys.empty() ? "nothing replayed" : keys)
                  << ")\n";
    } else {
        std::cout << "origin: none (header defaults)\n";
    }
    std::cout << "bytes: " << r.bytes.size() << (opt.zip ? " (zipped)" : "") << "\n"
              << "file: " << out_path << "\n";

    // ── warnings ────────────────────────────────────────────────────────────
    if (r.cells_dropped_height)
        std::cerr << "eden_export: warning: " << commas(r.cells_dropped_height)
                  << " cell(s) sit above the " << r.z_format() << " ceiling (height "
                  << r.z_ceiling << ") and were dropped. Pass --z 256 to keep them, if the"
                     " world is not already 256z.\n";
    if (r.cells_dropped_coord)
        std::cerr << "eden_export: warning: " << commas(r.cells_dropped_coord)
                  << " cell(s) lie outside the coordinate range a .eden directory can"
                     " address and were dropped.\n";
    if (r.signs_dropped)
        std::cerr << "eden_export: warning: " << r.signs_dropped
                  << " sign(s) cannot be expressed in file coordinates and were dropped.\n";
    if (r.painted_base_on_air)
        std::cerr << "eden_export: warning: " << commas(r.painted_base_on_air)
                  << " painted-base cell(s) sit where the base profile has no block; they"
                     " were written as air (there is nothing there to recolour).\n";
    if (store.reserved_coerced())
        std::cerr << "eden_export: warning: " << commas(store.reserved_coerced())
                  << " cell(s) in the source used the reserved block id 254 and were read"
                     " as mined air.\n";

    if (dry_run) {
        std::cout << "dry-run: nothing written\n";
        return 0;
    }

    // ── write ───────────────────────────────────────────────────────────────
    if (path_exists(out_path) && !force) {
        std::cerr << "eden_export: error: " << out_path
                  << " already exists. Pass --force to overwrite it.\n";
        return 1;
    }
    if (want_sidecar && path_exists(sidecar) && !force) {
        std::cerr << "eden_export: error: " << sidecar
                  << " already exists. Pass --force to overwrite it.\n";
        return 1;
    }

    // Stage both files, then publish both: a failure halfway leaves the previous
    // world (or nothing) in place, never a `.eden` without its signs.
    if (!stage_file(out_path, r.bytes.data(), r.bytes.size(), err)) {
        std::cerr << "eden_export: " << err << "\n";
        return 1;
    }
    if (want_sidecar && !stage_file(sidecar, r.sign_sidecar.data(), r.sign_sidecar.size(), err)) {
        std::cerr << "eden_export: " << err << "\n";
        discard_file(out_path);
        return 1;
    }
    if (!publish_file(out_path, err)) {
        std::cerr << "eden_export: " << err << "\n";
        if (want_sidecar) discard_file(sidecar);
        return 1;
    }
    if (want_sidecar && !publish_file(sidecar, err)) {
        std::cerr << "eden_export: " << err << "\n";
        return 1;
    }

    std::cout << "wrote " << out_path << "  " << commas(r.bytes.size()) << " bytes\n";
    if (want_sidecar)
        std::cout << "wrote " << sidecar << "  " << r.signs << " signs\n";
    std::cout << "import it back:  ./eden_import " << out_path << "\n";
    return 0;
}
