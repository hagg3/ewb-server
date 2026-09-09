// eden_import.cpp — convert a `.eden` world into a hostable server world.
//
// ROADMAP-SERVER stage 5.1 (Phase 5). Offline tool, run before the server
// starts; it never talks to a running server and never touches the network.
//
//   ./eden_import <world.eden> [--name NAME] [--air-fill diff|solid|full] ...
//
// Reads a `.eden` (ZIP-wrapped or raw), projects the two budgets that decide
// whether the result can be hosted, and — if they pass — writes
// `worlds/<name>/` with `eden_world.model`, `eden_signs.txt`, `eden_spawn.txt`
// and a `.gitignore`. See `docs/import.md`.
//
// This file owns argument parsing, file I/O and the summary. Every conversion
// decision lives in `eden_import.h`, which is pure and unit-tested
// (`eden_import_test.cpp`). Stage 5.2 adds the interactive prompt flow on top;
// everything here already has a flag, so that stage adds no new capability.
//
//   clang++ -std=c++17 -O2 -Wall eden_import.cpp -lz -o eden_import

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

#include "eden_file.h"
#include "eden_import.h"

using namespace ewb;

// ── small file helpers ──────────────────────────────────────────────────────

static bool read_file(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path + ": " + std::strerror(errno); return false; }
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) { err = "cannot size " + path; return false; }
    f.seekg(0, std::ios::beg);
    out.resize(size_t(n));
    if (n && !f.read(reinterpret_cast<char*>(out.data()), n)) {
        err = "short read on " + path;
        return false;
    }
    return true;
}

static bool read_text(const std::string& path, std::string& out, std::string& err) {
    std::vector<uint8_t> b;
    if (!read_file(path, b, err)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

static bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

static bool make_dirs(const std::string& path, std::string& err) {
    std::string acc;
    size_t i = 0;
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const size_t end = (slash == std::string::npos) ? path.size() : slash;
        acc = path.substr(0, end);
        i = end + 1;
        if (acc.empty()) continue;
        if (::mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) {
            err = "cannot create " + acc + ": " + std::strerror(errno);
            return false;
        }
        if (slash == std::string::npos) break;
    }
    return true;
}

// Temp file + rename, the same discipline `saveWorld()` uses: a crash or a full
// disk mid-write must never leave a truncated world where a whole one was.
static bool write_atomic(const std::string& path, const std::string& body, std::string& err) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot open " + tmp + ": " + std::strerror(errno); return false; }
        f.write(body.data(), std::streamsize(body.size()));
        f.flush();
        if (!f) {
            err = "write error on " + tmp;
            ::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename " + tmp + " -> " + path + " failed: " + std::strerror(errno);
        ::remove(tmp.c_str());
        return false;
    }
    return true;
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

static std::string pct(size_t v, size_t of) {
    if (!of) return "n/a";
    char b[32];
    snprintf(b, sizeof b, "%.1f%%", 100.0 * double(v) / double(of));
    return b;
}

/// Quote for a copy-pasteable shell line.
static std::string shq(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

// ── usage ───────────────────────────────────────────────────────────────────

static void usage() {
    std::cout <<
"eden_import — convert a .eden world into a hostable ewb-server world.\n"
"\n"
"  eden_import <world.eden> [options]\n"
"\n"
"Output selection\n"
"  --name NAME              world directory name (default: slug of the world's own name)\n"
"  --out DIR                write here instead of worlds/<name>/\n"
"  --force                  overwrite an existing output directory\n"
"  --dry-run                project and print the summary, write nothing\n"
"\n"
"Terrain\n"
"  --air-fill diff|solid|full   what becomes a cell (default: diff)\n"
"        diff   emit only voxels that differ from the client's base terrain.\n"
"               Faithful, including caves, at ~1%% of a full dump. Default.\n"
"        solid  emit solid blocks only. Sub-surface voids fill back in.\n"
"        full   emit every voxel of every saved chunk. Byte-faithful, assumes\n"
"               nothing about the client, and only viable on small worlds.\n"
"  --base-profile default|none|FILE   the terrain `diff` compares against\n"
"\n"
"Signs and spawn\n"
"  --signs FILE             sign sidecar (default: signs_<input>.dat next to the input)\n"
"  --no-signs               ignore signs entirely\n"
"  --spawn header|home|X,Y,Z|none   spawn written to eden_spawn.txt (default: header)\n"
"\n"
"Budgets\n"
"  --max-world-cells N      cell ceiling (default: 4000000, the server's own cap)\n"
"  --max-region-records N   worst-case single-REGION reply ceiling (default: 2000000; 0 = off)\n"
"  --region-radius N        match a server started with --region-radius (default: 224)\n"
"  --strict                 treat unknown block ids and out-of-range paints as errors\n"
"\n"
"  -h, --help               this text\n"
"\n"
"See docs/import.md for the format, the base-profile table and how to read the\n"
"two budget numbers.\n";
}

// ── main ────────────────────────────────────────────────────────────────────

namespace {

struct Args {
    std::string input;
    std::string name, out, signs, base_profile = "default", spawn_arg = "header";
    ImportOptions opt;
    bool force = false, dry_run = false, no_signs = false, help = false;
};

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
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string f = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "eden_import: " << f << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if      (f == "-h" || f == "--help") a.help = true;
        else if (f == "--name")            a.name = next("a name");
        else if (f == "--out")             a.out = next("a directory");
        else if (f == "--force")           a.force = true;
        else if (f == "--dry-run")         a.dry_run = true;
        else if (f == "--air-fill") {
            const std::string v = next("diff|solid|full");
            if (!eden_parse_air_fill(v, a.opt.air_fill)) {
                std::cerr << "eden_import: unknown --air-fill " << v << " (diff|solid|full)\n";
                return 2;
            }
        }
        else if (f == "--base-profile")    a.base_profile = next("default|none|FILE");
        else if (f == "--signs")           a.signs = next("a file");
        else if (f == "--no-signs")        a.no_signs = true;
        else if (f == "--spawn")           a.spawn_arg = next("header|home|X,Y,Z|none");
        else if (f == "--strict")          a.opt.strict = true;
        else if (f == "--max-world-cells") {
            if (!parse_size(next("a count"), a.opt.max_world_cells)) {
                std::cerr << "eden_import: --max-world-cells needs a non-negative integer\n";
                return 2;
            }
        }
        else if (f == "--max-region-records") {
            if (!parse_size(next("a count"), a.opt.max_region_records)) {
                std::cerr << "eden_import: --max-region-records needs a non-negative integer\n";
                return 2;
            }
        }
        else if (f == "--region-radius")   a.opt.region_radius = std::atoi(next("a radius").c_str());
        else if (!f.empty() && f[0] == '-') {
            std::cerr << "eden_import: unknown option " << f << " (try --help)\n";
            return 2;
        }
        else if (a.input.empty())          a.input = f;
        else {
            std::cerr << "eden_import: unexpected argument " << f << "\n";
            return 2;
        }
    }

    if (a.help || a.input.empty()) {
        usage();
        return a.help ? 0 : 2;
    }
    if (a.opt.region_radius < 16 || a.opt.region_radius > 4096) {
        std::cerr << "eden_import: --region-radius out of range (16..4096)\n";
        return 2;
    }

    // ── the base profile ────────────────────────────────────────────────────
    if (a.base_profile == "default")   a.opt.profile = eden_default_profile();
    else if (a.base_profile == "none") a.opt.profile = eden_empty_profile();
    else {
        std::string text, err;
        if (!read_text(a.base_profile, text, err)) {
            std::cerr << "eden_import: " << err << "\n";
            return 1;
        }
        if (!eden_parse_profile(text, a.opt.profile, err)) {
            std::cerr << "eden_import: " << a.base_profile << ": " << err << "\n";
            return 1;
        }
    }
    if (a.opt.air_fill != AirFill::Diff && a.base_profile != "default")
        std::cerr << "eden_import: note: --base-profile only affects --air-fill diff\n";

    // ── load ────────────────────────────────────────────────────────────────
    std::vector<uint8_t> raw;
    std::string err;
    if (!read_file(a.input, raw, err)) {
        std::cerr << "eden_import: " << err << "\n";
        return 1;
    }
    EdenWorld world;
    try {
        world = eden_load(raw);
    } catch (const std::exception& e) {
        std::cerr << "eden_import: " << a.input << ": " << e.what() << "\n";
        return 1;
    }
    raw.clear();
    raw.shrink_to_fit();

    // ── signs: sidecar wins over the inline trailer ─────────────────────────
    std::vector<EdenSign> src_signs;
    std::string sign_source = "none";
    if (!a.no_signs) {
        // The game names the sidecar `signs_<worldfile>.dat` beside the world.
        std::string sidecar = a.signs;
        if (sidecar.empty()) {
            const size_t slash = a.input.find_last_of('/');
            const std::string dir  = (slash == std::string::npos) ? "" : a.input.substr(0, slash + 1);
            const std::string base = (slash == std::string::npos) ? a.input : a.input.substr(slash + 1);
            const std::string cand = dir + "signs_" + base + ".dat";
            if (path_exists(cand)) sidecar = cand;
        }
        if (!sidecar.empty()) {
            std::vector<uint8_t> sb;
            if (!read_file(sidecar, sb, err)) {
                std::cerr << "eden_import: " << err << "\n";
                return 1;
            }
            src_signs = eden_parse_signs(sb);
            sign_source = "sidecar " + sidecar;
            if (src_signs.empty())
                std::cerr << "eden_import: warning: " << sidecar
                          << " holds no readable sign records\n";
            if (!world.signs.empty())
                std::cerr << "eden_import: warning: the world also carries " << world.signs.size()
                          << " inline sign(s); the sidecar wins, as it does in the game\n";
        } else if (!world.signs.empty()) {
            src_signs = world.signs;
            sign_source = "inline trailer";
        }
    }
    size_t signs_dropped = 0;
    const std::vector<Sign> signs = eden_convert_signs(src_signs, signs_dropped);

    // ── spawn ───────────────────────────────────────────────────────────────
    bool have_spawn = true;
    Spawn spawn;
    std::string spawn_source = a.spawn_arg;
    if (a.spawn_arg == "header")     spawn = eden_spawn_from(world.hdr, false);
    else if (a.spawn_arg == "home")  spawn = eden_spawn_from(world.hdr, true);
    else if (a.spawn_arg == "none")  have_spawn = false;
    else {
        if (std::sscanf(a.spawn_arg.c_str(), "%f,%f,%f", &spawn.x, &spawn.y, &spawn.z) != 3) {
            std::cerr << "eden_import: --spawn wants header|home|X,Y,Z|none\n";
            return 2;
        }
        spawn_source = "explicit";
    }
    if (have_spawn && !eden_spawn_in_range(spawn)) {
        std::cerr << "eden_import: warning: spawn (" << spawn.x << ", " << spawn.y << ", "
                  << spawn.z << ") is outside the server's coordinate range; omitting"
                     " eden_spawn.txt\n";
        have_spawn = false;
    }

    // ── project ─────────────────────────────────────────────────────────────
    const ImportProjection p = eden_project(world, a.opt);

    const std::string world_name = a.name.empty()
                                       ? eden_slug(world.hdr.name.empty() ? a.input : world.hdr.name)
                                       : a.name;
    const std::string outdir = a.out.empty() ? "worlds/" + eden_slug(world_name) : a.out;

    // ── summary ─────────────────────────────────────────────────────────────
    std::cout << "world " << (world.hdr.name.empty() ? "(unnamed)" : "\"" + world.hdr.name + "\"")
              << "  (version " << world.hdr.version << ", " << (world.z_ceiling + 1) << "z, "
              << world.chunks.size() << " saved chunk" << (world.chunks.size() == 1 ? "" : "s")
              << ", " << src_signs.size() << " sign" << (src_signs.size() == 1 ? "" : "s")
              << " from " << sign_source << ")\n";
    if (p.empty) {
        std::cout << "  bounding box  (nothing to emit)\n";
    } else {
        std::cout << "  bounding box  x " << p.x0 << ".." << p.x1
                  << "   y " << p.y0 << ".." << p.y1
                  << "   z " << p.z0 << ".." << p.z1 << "   (server coords)\n";
    }
    std::cout << "  strategy      " << eden_air_fill_name(a.opt.air_fill)
              << " (base profile: " << a.base_profile << ")\n"
              << "  cells         " << commas(p.cells) << "  of "
              << commas(a.opt.max_world_cells) << " cap  ("
              << pct(p.cells, a.opt.max_world_cells) << ")\n"
              << "  worst REGION  " << commas(p.worst_region.records) << " records";
    if (p.worst_region.records) {
        std::cout << "  at x " << p.worst_region.x0 << ".." << p.worst_region.x1
                  << ", z " << p.worst_region.z0 << ".." << p.worst_region.z1;
    }
    std::cout << "\n                (largest reply ever captured from the real server: "
              << commas(EDEN_REGION_RECORDS_OBSERVED) << " records)\n";
    std::cout << "  signs         " << signs.size() << " convertible";
    if (signs_dropped) std::cout << ", " << signs_dropped << " out of range and dropped";
    std::cout << "\n  spawn         ";
    if (have_spawn) {
        char b[96];
        snprintf(b, sizeof b, "%.2f, %.2f, %.2f", spawn.x, spawn.y, spawn.z);
        std::cout << b << "  (" << spawn_source << ")\n";
    } else {
        std::cout << "none\n";
    }
    {
        std::cout << "  sky colours   ";
        for (int i = 0; i < 16; ++i)
            std::cout << (i ? " " : "") << int(world.hdr.skycolors[i]);
        std::cout << "\n                (informational — the wire protocol carries no sky message)\n";
    }

    // ── verdicts ────────────────────────────────────────────────────────────
    bool fatal = false;
    if (p.sentinel_cells) {
        std::cerr << "eden_import: error: " << commas(p.sentinel_cells)
                  << " voxel(s) have block type 255, which collides with the server's"
                     " painted-base sentinel — such cells would be silently dropped from"
                     " every REGION reply. First at x " << p.sentinel_at[0] << ", y "
                  << p.sentinel_at[1] << ", z " << p.sentinel_at[2] << ".\n";
        fatal = true;
    }
    if (p.bad_type_cells) {
        std::cerr << "eden_import: " << (a.opt.strict ? "error" : "warning") << ": "
                  << commas(p.bad_type_cells) << " voxel(s) use a block id above "
                  << MAX_BLOCK_TYPE << ". Kept verbatim — the client may know blocks this"
                     " tool does not.\n";
        if (a.opt.strict) fatal = true;
    }
    if (p.bad_paint_cells) {
        std::cerr << "eden_import: " << (a.opt.strict ? "error" : "warning") << ": "
                  << commas(p.bad_paint_cells) << " voxel(s) carry a paint above "
                  << int(CELL_MAX_PAINT) << ". Kept in the model file, but the server drops"
                     " an out-of-palette paint on the wire, so they will render unpainted.\n";
        if (a.opt.strict) fatal = true;
    }
    if (p.cells > a.opt.max_world_cells) {
        std::cerr << "eden_import: error: " << commas(p.cells) << " cells exceeds the "
                  << commas(a.opt.max_world_cells) << " cap. Either import with"
                     " --air-fill diff, or raise both ceilings: eden_import"
                     " --max-world-cells " << p.cells << " and edenserver --max-world-cells "
                  << p.cells << ".\n";
        fatal = true;
    }
    if (a.opt.max_region_records && p.worst_region.records > a.opt.max_region_records) {
        std::cerr << "eden_import: error: a single REGION reply would carry "
                  << commas(p.worst_region.records) << " records, over the "
                  << commas(a.opt.max_region_records) << " ceiling ("
                  << (p.worst_region.records / EDEN_REGION_RECORDS_OBSERVED)
                  << "x the largest reply ever captured). Clients re-request regions every"
                     " ~750 ms, so this stalls the server rather than merely loading it."
                     " Use --air-fill diff, or --max-region-records "
                  << p.worst_region.records << " to proceed anyway.\n";
        fatal = true;
    } else if (p.worst_region.records > EDEN_REGION_RECORDS_OBSERVED) {
        std::cerr << "eden_import: warning: the worst single REGION reply ("
                  << commas(p.worst_region.records) << " records) is larger than any reply"
                     " ever captured from the real server (" << commas(EDEN_REGION_RECORDS_OBSERVED)
                  << "). Watch the first join.\n";
    }
    if (fatal) return 1;

    if (a.dry_run) {
        std::cout << "\n--dry-run: nothing written. Output would have been " << outdir << "/\n";
        return 0;
    }

    // ── write ───────────────────────────────────────────────────────────────
    if (path_exists(outdir) && !a.force) {
        std::cerr << "eden_import: error: " << outdir
                  << " already exists. Pass --force to overwrite it.\n";
        return 1;
    }
    if (!make_dirs(outdir, err)) {
        std::cerr << "eden_import: " << err << "\n";
        return 1;
    }

    // The .gitignore goes down first: if a later write fails, whatever landed is
    // already excluded from `git add -A`.
    if (!write_atomic(outdir + "/.gitignore", eden_output_gitignore(), err) ||
        !write_atomic(outdir + "/eden_world.model", eden_build_model(world, a.opt), err) ||
        !write_atomic(outdir + "/eden_signs.txt", eden_build_signs(signs), err)) {
        std::cerr << "eden_import: " << err << "\n";
        return 1;
    }
    if (have_spawn && !write_atomic(outdir + "/eden_spawn.txt", eden_format_spawn(spawn), err)) {
        std::cerr << "eden_import: " << err << "\n";
        return 1;
    }

    std::cout << "\nwrote " << outdir << "/eden_world.model   " << commas(p.cells) << " cells\n"
              << "wrote " << outdir << "/eden_signs.txt     " << signs.size() << " signs\n";
    if (have_spawn)
        std::cout << "wrote " << outdir << "/eden_spawn.txt     " << eden_format_spawn(spawn);
    std::cout << "wrote " << outdir << "/.gitignore         (worlds/ is tracked — this keeps "
                 "the world out of commits)\n";
    std::cout << "\nrun it:  ./host_world.sh " << shq(outdir + "/eden_world.model") << " "
              << shq(world.hdr.name.empty() ? world_name : world.hdr.name) << " 27015\n";
    if (have_spawn)
        std::cout << "         (eden_spawn.txt needs a server built from ROADMAP-SERVER stage 5.3;"
                     " until then it is inert)\n";
    return 0;
}
