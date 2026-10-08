# ewb-server

A self-hosted multiplayer server for **Eden** (Eden World Builder). It speaks the game's own
network protocol, so an **unmodified retail client** can join a world you run yourself — on your
laptop, your LAN, or a cheap VPS.

It is a single C++17 program with no dependencies beyond zlib, plus a few small companion tools
for importing and exporting worlds, listing servers, and administering a running world.

> This is an independent community project. It is not affiliated with or endorsed by Eden's
> developer. You need your own copy of the game to connect.

## What you get

**Hosting**
- Join, chat, building, mining, painting, TNT and fire, and movement sync for up to 64 players.
- Worlds persist to disk: saved every 15 seconds and whenever a player leaves, written
  atomically (temp file → `fsync` → rename), so a crash or power cut cannot leave a half-written
  world.
- A password, per-IP connection and authentication limits, rate limits on edits, movement and
  chat, and hard caps on world size and per-client memory. The server treats every byte from a
  client as untrusted.

**Running a community**
- **Protected zones** (anti-grief): rectangular regions where building, mining and burning are
  refused and instantly reverted, with an audit trail.
- **Player identity**: an operator can issue a PIN for a name, so nobody else can pose as that
  player or inherit their permissions.
- **Permission levels** (visitor → builder → admin) gate every command.
- Kick, ban (by name or IP), a message of the day, and `--tnt off` / `--fire off` switches.
- Choose where returning players land: the world spawn, their last position, or a fixed point
  (`--rejoin`). `/last` jumps back to where the previous session ended.

**In-game building commands** (typed into chat) — a WorldEdit-style toolkit: `//set`, `//replace`,
`//walls`, `//sphere`, `//cyl`, `//copy` / `//paste` / `//rotate`, `//paint`, `//undo` / `//redo`,
`/tp`, `/msg`, and name lookups such as `//set stone` or `/searchblocks`. Every command is bounded
(selection size, rate, undo memory). See [docs/commands.md](docs/commands.md).

**Tools**

| Tool | What it does |
|---|---|
| `edenserver` | The game server. |
| `edenmatch` | A standalone matchmaker, so a server can appear in the in-game Server Browser (LAN / private lists). |
| `eden_import` | Converts a saved `.eden` world into a world this server can host. |
| `eden_export` | Converts a hosted world back into a `.eden` you can open in the game. |
| `edenctl` | Command-line client for the server's local operator socket (kick, ban, save, say, edit blocks, …). |
| `admin/` | `edenadmin`, an optional local web GUI for managing a server, local or over SSH (separate Go module). |
| `ops/` | systemd units, backup timers, fail2ban filter and an install guide for a Linux host. |

## Quick start

You need macOS or Linux (Windows: use WSL), a C++17 compiler (`clang++` or `g++`), and zlib.

```bash
# Debian / Ubuntu:  sudo apt install build-essential zlib1g-dev
# macOS:            xcode-select --install

./build_server.sh                # builds everything and runs the offline test suites

mkdir -p ~/edenworlds/myworld && cd ~/edenworlds/myworld
/path/to/ewb-server/edenserver   # listens on TCP 27015; world files are created on first save
```

Already have a saved world? Convert it first, then run the server from the new directory:

```bash
./eden_import ~/Downloads/myworld.eden        # writes worlds/myworld/
./eden_import ~/Downloads/myworld.eden --dry-run   # preview the cost without writing
```

**Connecting.** The retail game has no "connect to IP" box. Point it at your server with the
`EDEN_MP_AUTOJOIN` environment variable:

```bash
EDEN_MP_AUTOJOIN=127.0.0.1:27015 <path-to-the-game-executable>
```

On macOS, set it for the login session first with
`launchctl setenv EDEN_MP_AUTOJOIN "127.0.0.1:27015"`. The retail client cannot yet be pointed at
a different matchmaker, so `edenmatch` currently suits LAN play, testing and friends-lists rather
than the public browser. Full steps and troubleshooting: [docs/quickstart.md](docs/quickstart.md).

Open **TCP 27015** in your firewall (and forward it on your router) for players outside your LAN.

## How it works

Eden's multiplayer is a line-oriented, `:`-delimited text protocol over TCP. A client sends
things like `JOIN`, `MSG`, `ACTION:x:y:z:mode[:type]` (build / mine / burn / paint), position and
velocity updates, `REGION:x:z` and `SIGNQ`; the server broadcasts movement and edits, and answers
with `SPAWN`, `SIGNP` (signs) and `SNAPZ` (terrain).

The interesting part is the world. Eden terrain is procedural, so the server stores only
**what players changed**, never the whole world. When a client asks for a `REGION`, the server
finds the edited cells in a bounded box around it, compresses them with raw DEFLATE, base64s the
result, and streams it back as `SNAPZ` frames. Anything not stored is drawn by the client from the
same base terrain profile, which is why a world that is mostly untouched costs almost nothing.

Coordinates are centred on 65536 in x/z, with y as height; block type 0 is air.

```
 retail client ──TCP──►  edenserver ── autosave ──► eden_world.model   (edits only)
                           │  ▲                     eden_players.txt   (last positions)
        edenmatch ◄────────┘  │                     eden_signs.txt, eden_zones.txt, …
   (optional Server Browser)  │
                              └── local unix socket ◄── edenctl / edenadmin
```

Design choices worth knowing:

- **One translation unit plus pure headers.** `server_posix.cpp` is the server; protocol and
  world logic live in small header-only modules (`region_query.h`, `snapz_codec.h`, `zones.h`,
  `worldedit.h`, `world_store.h`, …) that are each covered by an offline test suite.
- **Operator commands never touch the game wire.** They go over a `0600` unix socket, so the
  filesystem permissions are the authentication and a connected player cannot reach them.
- **Output is queued per client** with byte caps, so one slow or hostile connection cannot stall
  everyone else or exhaust memory.

The deep dives: [architecture](docs/architecture.md) · [wire protocol](docs/protocol.md) ·
[matchmaker](docs/matchmaker.md).

## Configuration

Everything is a command-line flag, with sensible defaults (`--port`, `--name`, `--password`,
`--world`, `--rejoin`, `--tnt`, `--fire`, rate and size limits, …). Under systemd the same
settings come from `/etc/edenserver.conf`. The exhaustive list, with on-disk file formats, is
[docs/configuration.md](docs/configuration.md).

For a Linux host that stays up — dedicated user, systemd unit(s) for one or many worlds, backups,
fail2ban — follow [ops/INSTALL.md](ops/INSTALL.md).

## Testing

`./build_server.sh` builds the binaries and then runs the offline suites (codec, region queries,
protocol and hardening, control socket, world editing, `.eden` parse / import / export, world
store, zones, auth, burn rules, matchmaker). A failing suite fails the build.

Four live socket tests (`phase3_live_test.py`, `phase7_live_test.py`, `phase8_live_test.py`,
`phase10_live_test.py`) start real server processes and attack their command surfaces, so the
build does not run them. Run them by hand before exposing a server publicly.

## Documentation

| | |
|---|---|
| [Quickstart](docs/quickstart.md) | Build, run, connect. |
| [Configuration](docs/configuration.md) | Every flag, environment variable and file format. |
| [Commands](docs/commands.md) | Operator socket and in-game chat commands. |
| [Importing](docs/import.md) / [Exporting](docs/export.md) | Moving worlds in and out of `.eden` files. |
| [Architecture](docs/architecture.md) | Threads, locks, world model, persistence. |
| [Protocol](docs/protocol.md) | Every message, the join sequence, limits. |
| [Matchmaker](docs/matchmaker.md) | Server Browser listing. |
| [Operations](ops/INSTALL.md) | Running as a service. |
| [Admin GUI](admin/README.md) | The optional `edenadmin` tool. |

## Repository layout

```
server_posix.cpp   the server               edenmatch.cpp     the matchmaker
eden_import.*      .eden → world            eden_export.*     world → .eden
eden_file.h        .eden file parser        worldedit.h       in-game commands
*.h                pure protocol/world logic, each with a matching *_test.cpp
docs/  ops/  admin/  worlds/ (sample worlds)  testdata/
```

The server began as a port of a small Windows-only Winsock program; it is now POSIX-only
(Windows users: WSL).

## Contributing

Issues and pull requests are welcome. Two house rules: new protocol logic goes in a header with
an offline test suite wired into `build_server.sh`, and the matching page in `docs/` is updated in
the same commit ([docs/README.md](docs/README.md) maps source files to docs). Please don't commit
real player data, credentials, or captured traffic.
