#!/bin/bash
# Create a multiplayer server that hosts a specific (single-player) world file,
# registered with a matchmaker so it shows up in the in-game server browser.
# Run ./edenmatch on the matchmaker host first (see docs/matchmaker.md).
#
# Usage:
#   ./host_world.sh <worldFile> [name] [port] [password] [matchmakerHost]
#
# Examples:
#   ./host_world.sh myworld.edits "My World"            27015
#   ./host_world.sh myworld.edits "My World"            27015  secret   192.168.1.170
#
# The world file is the server's edit log (created/saved as it is played). Copy a
# saved world's .edits file here to "create a server from" it.
cd "$(dirname "$0")"
[ -x ./edenserver ] || ./build_server.sh

WORLD="${1:-eden_world.edits}"
NAME="${2:-Eden Server}"
PORT="${3:-27015}"
PASSWORD="${4:-}"
MM="${5:-127.0.0.1}"

ARGS=(--world "$WORLD" --name "$NAME" --port "$PORT" --matchmaker "$MM")
[ -n "$PASSWORD" ] && ARGS+=(--password "$PASSWORD")

echo "Hosting world '$WORLD' as \"$NAME\" on port $PORT (matchmaker $MM)${PASSWORD:+ [password-protected]}"
exec ./edenserver "${ARGS[@]}"
