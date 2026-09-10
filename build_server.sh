#!/bin/bash
# Build the standalone Eden multiplayer server (macOS/Linux).
#
#   ./build_server.sh                builds the server + tools + runs the offline suites
#   ./build_server.sh --with-admin   also builds admin/ (the edenadmin operator GUI)
#
# --with-admin is opt-in: admin/ needs a Go toolchain, and a VPS host that only
# wants to run the server should not have its build broken by a missing `go`.
# admin/build.sh stands alone for Mac use. See admin/README.md.
set -e
cd "$(dirname "$0")"

WITH_ADMIN=0
for arg in "$@"; do
    case "$arg" in
        --with-admin) WITH_ADMIN=1 ;;
        *) echo "build_server.sh: unknown argument $arg" >&2; exit 1 ;;
    esac
done

# Compiler: honour $CXX if set, else prefer clang++ (macOS / the dev box), else
# fall back to g++ (`apt install build-essential` on Debian/Ubuntu gives g++, not
# clang++). Both build this tree with -std=c++17.
if [ -z "${CXX:-}" ]; then
    if command -v clang++ >/dev/null 2>&1; then
        CXX=clang++
    elif command -v g++ >/dev/null 2>&1; then
        CXX=g++
    else
        echo "build_server.sh: no C++ compiler found." >&2
        echo "  Debian/Ubuntu:  apt install build-essential zlib1g-dev" >&2
        echo "  macOS:          xcode-select --install" >&2
        exit 1
    fi
fi
echo "Using compiler: $CXX"

# -lz: raw DEFLATE for the SNAPZ region-streaming encoder (snapz_codec.h,
# ROADMAP-SERVER stage 1.2). On Debian/Ubuntu: apt install build-essential zlib1g-dev.
#
# WARNING: the resulting ./edenserver is native to THIS machine's CPU. Do not
# copy an arm64 build (Apple Silicon, ARM VPS) to an x86-64 VPS or vice versa —
# it will not run. Build on the box you deploy to (see ops/INSTALL.md).
"$CXX" -std=c++17 -O2 -pthread server_posix.cpp -lz -o edenserver
echo "Built ./edenserver  —  run with: ./edenserver [port]   (default 27015)"

# The standalone matchmaker (ROADMAP-SERVER Phase 2). Same single-TU + pure-header
# style as the server; no zlib. Point a server at it with --matchmaker.
"$CXX" -std=c++17 -O2 -pthread edenmatch.cpp -o edenmatch
echo "Built ./edenmatch  —  run with: ./edenmatch [--port 27020]"

# Offline SNAPZ codec round-trip (no server needed).
"$CXX" -std=c++17 -O2 -Wall snapz_codec_test.cpp -lz -o snapz_codec_test
./snapz_codec_test
echo "Built ./snapz_codec_test  —  SNAPZ raw-DEFLATE/base64 round-trip"

# Offline REGION query checks: reply geometry, Cell -> record table, frame split
# (ROADMAP-SERVER stage 1.1).
"$CXX" -std=c++17 -O2 -Wall region_test.cpp -lz -o region_test
./region_test
echo "Built ./region_test  —  REGION box / encoding / framing"

# Offline sign + spawn + hardening checks: eden_signs.txt / eden_spawn.txt
# parsing, SIGNP formatting, username/ACTION validation, token bucket, connect
# limiter, constant-time password compare + failed-auth limiter
# (ROADMAP-SERVER stages 1.5 + 1.7 + 1.10 + 5.3). No zlib needed.
"$CXX" -std=c++17 -O2 -Wall protocol_test.cpp -o protocol_test
./protocol_test
echo "Built ./protocol_test  —  signs / spawn / names / rate limits"

# Offline control-surface checks: line grammar, the command table, the persisted
# ban list + op-level files, fill volume arithmetic (ROADMAP-SERVER stage 3.2),
# plus the derived fill cap and the connection flood guard (stage 3.4).
# No zlib needed.
"$CXX" -std=c++17 -O2 -Wall control_test.cpp -o control_test
./control_test
echo "Built ./control_test  —  control socket grammar / bans / ops / fill bounds / flood guard"

# Offline Tier 2 command-surface checks: the dispatch table and its permission
# floors, the line grammar, the selection cap, the byte-bounded undo store and
# the clipboard rotation (ROADMAP-SERVER stage 3.3). No zlib needed.
"$CXX" -std=c++17 -O2 -Wall worldedit_test.cpp -o worldedit_test
./worldedit_test
echo "Built ./worldedit_test  —  WorldEdit table / permissions / selection cap / undo"

# Offline name-table checks: the community block / paint / character name tables,
# their round-trips, aliases and sentinels, and the /searchblocks + /searchcolors
# substring search (ROADMAP-SERVER stage 3.6). No zlib needed.
"$CXX" -std=c++17 -O2 -Wall eden_names_test.cpp -o eden_names_test
./eden_names_test
echo "Built ./eden_names_test  —  block / paint / character name tables + search"

# Offline .eden world-format parser checks: the 192-byte header, chunk-size
# detection (version / creature-gap / min-gap), the directory coordinate gate,
# per-chunk derived spans + bounded voxel reads, the sidecar + inline sign
# parsers, and the ZIP wrapper (member select + decompression-bomb cap)
# (ROADMAP-SERVER stage 5.0). Links -lz for the ZIP inflate.
"$CXX" -std=c++17 -O2 -Wall eden_file_test.cpp -lz -o eden_file_test
./eden_file_test
echo "Built ./eden_file_test  —  .eden header / chunk detection / spans / signs / ZIP"

# The .eden -> server-world converter (ROADMAP-SERVER stage 5.1). Offline tool;
# run it before the server starts. Links -lz through eden_file.h's ZIP inflate.
"$CXX" -std=c++17 -O2 -Wall eden_import.cpp -lz -o eden_import
echo "Built ./eden_import  —  run with: ./eden_import <world.eden> [--help]"

# Offline import-core checks: the base-terrain profile, the diff/solid/full
# emitter, the axis rename for blocks and signs, the cell + worst-case-REGION
# projections, and the golden end-to-end (testdata/carved_64z.model). The CLI
# half shells out to ./eden_import, built just above (stage 5.1).
"$CXX" -std=c++17 -O2 -Wall eden_import_test.cpp -lz -o eden_import_test
./eden_import_test
echo "Built ./eden_import_test  —  base profile / emitter / budgets / golden model"

# Offline matchmaker checks: name sanitising, REGISTER parsing, SERVER: row /
# LIST formatting, the TTL registry (ROADMAP-SERVER Phase 2). No zlib needed.
"$CXX" -std=c++17 -O2 -Wall matchmaker_test.cpp -o matchmaker_test
./matchmaker_test
echo "Built ./matchmaker_test  —  matchmaker REGISTER / LIST / registry"

# edenadmin, the local operator GUI (ROADMAP-SERVER Phase 6). Opt-in: --with-admin.
# Its own offline Go suite (quoting boundary, per-verb argv for both transports,
# profile round-trip, the HTTP guard rejections) runs inside admin/build.sh.
if [ "$WITH_ADMIN" -eq 1 ]; then
    echo
    echo "--- admin/ (edenadmin operator GUI) ---"
    ./admin/build.sh
fi

# Not run here (they bind a port and spawn server processes) — run by hand:
#   python3 phase1_live_test.py   —  join order / PONG / SIGNQ / limits, over real sockets
#   python3 phase3_live_test.py   —  adversarial pass at the Tier 1 + Tier 2 command
#                                    surfaces: oversized selections, level escalation,
#                                    undo growth, flooding, malformed arguments
