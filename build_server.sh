#!/bin/bash
# Build the standalone Eden multiplayer server (macOS/Linux).
set -e
cd "$(dirname "$0")"

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

# Offline sign + hardening checks: eden_signs.txt parsing, SIGNP formatting,
# username/ACTION validation, token bucket, connect limiter, constant-time
# password compare + failed-auth limiter
# (ROADMAP-SERVER stages 1.5 + 1.7 + 1.10). No zlib needed.
"$CXX" -std=c++17 -O2 -Wall protocol_test.cpp -o protocol_test
./protocol_test
echo "Built ./protocol_test  —  signs / names / rate limits"

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

# Offline matchmaker checks: name sanitising, REGISTER parsing, SERVER: row /
# LIST formatting, the TTL registry (ROADMAP-SERVER Phase 2). No zlib needed.
"$CXX" -std=c++17 -O2 -Wall matchmaker_test.cpp -o matchmaker_test
./matchmaker_test
echo "Built ./matchmaker_test  —  matchmaker REGISTER / LIST / registry"

# Not run here (they bind a port and spawn server processes) — run by hand:
#   python3 phase1_live_test.py   —  join order / PONG / SIGNQ / limits, over real sockets
#   python3 phase3_live_test.py   —  adversarial pass at the Tier 1 + Tier 2 command
#                                    surfaces: oversized selections, level escalation,
#                                    undo growth, flooding, malformed arguments
