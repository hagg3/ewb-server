#!/bin/bash
# Create a multiplayer server that hosts a specific (single-player) world file,
# registered with a matchmaker so it shows up in the in-game server browser.
# Run ./edenmatch on the matchmaker host first (see docs/matchmaker.md).
#
# Usage:
#   ./host_world.sh <worldFile> [name] [port] [password] [matchmakerHost]
#
# The password is handed to the server through its environment (EDEN_PASSWORD),
# not its command line, so `ps` does not show it. Typing it as the 4th argument
# still puts it in your shell history: to avoid that, leave the argument empty and
# export EDEN_PASSWORD (or `read -rs EDEN_PASSWORD; export EDEN_PASSWORD`) first.
#
# Examples:
#   ./host_world.sh myworld.edits "My World"            27015
#   ./host_world.sh myworld.edits "My World"            27015  secret   192.168.1.170
#
# The world file is the server's edit log (created/saved as it is played). Copy a
# saved world's .edits file here to "create a server from" it, or convert a .eden
# world with ./eden_import (see docs/import.md) and pass the eden_world.model it
# writes. An eden_signs.txt or eden_spawn.txt sitting next to the world file is
# loaded too.
cd "$(dirname "$0")"
[ -x ./edenserver ] || ./build_server.sh

WORLD="${1:-eden_world.edits}"
NAME="${2:-Eden Server}"
PORT="${3:-27015}"
PASSWORD="${4:-${EDEN_PASSWORD:-}}"
MM="${5:-127.0.0.1}"

ARGS=(--world "$WORLD" --name "$NAME" --port "$PORT" --matchmaker "$MM")
# (No --password: the secret goes in the environment; see the note at the top.)
if [ -n "$PASSWORD" ]; then export EDEN_PASSWORD="$PASSWORD"; else unset EDEN_PASSWORD; fi

# --signs defaults to ./eden_signs.txt, which is the wrong file when the world
# lives in its own directory (as everything eden_import writes does). Point it
# at the sidecar next to the world whenever there is one.
WORLD_DIR="$(dirname "$WORLD")"
[ -f "$WORLD_DIR/eden_signs.txt" ] && ARGS+=(--signs "$WORLD_DIR/eden_signs.txt")

# eden_spawn.txt (the default spawn eden_import writes) is picked up automatically
# by the server from the world's own directory — nothing to pass here.

# Anything after the fifth positional argument is forwarded to edenserver as-is,
# e.g. ./host_world.sh w "N" 27015 "" 127.0.0.1 --max-world-cells 8000000
ARGS+=("${@:6}")

echo "Hosting world '$WORLD' as \"$NAME\" on port $PORT (matchmaker $MM)${PASSWORD:+ [password-protected]}"
exec ./edenserver "${ARGS[@]}"
