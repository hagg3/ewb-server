#!/bin/bash
# Build the standalone Eden multiplayer server (macOS/Linux).
set -e
cd "$(dirname "$0")"

# -lz: raw DEFLATE for the SNAPZ region-streaming encoder (snapz_codec.h,
# ROADMAP-SERVER stage 1.2). On Debian/Ubuntu: apt install zlib1g-dev.
clang++ -std=c++17 -O2 -pthread server_posix.cpp -lz -o edenserver
echo "Built ./edenserver  —  run with: ./edenserver [port]   (default 27015)"

# Offline SNAPZ codec round-trip (no server needed).
clang++ -std=c++17 -O2 -Wall snapz_codec_test.cpp -lz -o snapz_codec_test
./snapz_codec_test
echo "Built ./snapz_codec_test  —  SNAPZ raw-DEFLATE/base64 round-trip"

# Offline REGION query checks: reply geometry, Cell -> record table, frame split
# (ROADMAP-SERVER stage 1.1).
clang++ -std=c++17 -O2 -Wall region_test.cpp -lz -o region_test
./region_test
echo "Built ./region_test  —  REGION box / encoding / framing"

# Offline sign + hardening checks: eden_signs.txt parsing, SIGNP formatting,
# username/ACTION validation, token bucket, connect limiter
# (ROADMAP-SERVER stages 1.5 + 1.7). No zlib needed.
clang++ -std=c++17 -O2 -Wall protocol_test.cpp -o protocol_test
./protocol_test
echo "Built ./protocol_test  —  signs / names / rate limits"

# Not run here (it binds port 27099 and spawns server processes):
#   python3 phase1_live_test.py   —  join order / PONG / SIGNQ / limits, over real sockets
