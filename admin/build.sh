#!/bin/sh
# Build + test edenadmin, the local operator GUI for the standalone Eden server.
#
# Offline: no server, no network, no VPS. `go test ./...` covers the quoting
# boundary, every verb's argv for both transports, the profile round-trip, and
# the four HTTP guard rejections.
#
# This is NOT run by build_server.sh's default path — a VPS host that only wants
# to run the server should not need a Go toolchain. `build_server.sh --with-admin`
# opts in; otherwise run this directly.
set -eu
cd "$(dirname "$0")"

if ! command -v go >/dev/null 2>&1; then
    echo "admin/build.sh: no Go toolchain found." >&2
    echo "  macOS:  brew install go" >&2
    echo "  Linux:  apt install golang  (or https://go.dev/dl/)" >&2
    exit 1
fi

echo "go vet ./..."
go vet ./...

echo "go test ./..."
go test ./...

echo "go build -o edenadmin ./cmd/edenadmin"
go build -o edenadmin ./cmd/edenadmin

echo "Built ./admin/edenadmin  —  run with: ./admin/edenadmin  (opens http://127.0.0.1:<port>)"
