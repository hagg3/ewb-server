# Quickstart — host your own Eden world

**Audience:** end users who want to run a server and connect an unmodified retail Eden client.

This is the minimal path. Every option is in [configuration.md](configuration.md).

## 1. Requirements

- **macOS or Linux.** The server is POSIX; on Windows, use WSL.
- A C++17 compiler — `clang++` or `g++`.
- **zlib**, including headers.

```bash
# Debian / Ubuntu
sudo apt install build-essential zlib1g-dev

# macOS (zlib ships with the SDK)
xcode-select --install
```

## 2. Build

```bash
./build_server.sh
```

That produces `./edenserver`, `./edenmatch` (the matchmaker) and `./eden_import` (the `.eden`
world converter), then builds and runs the offline test suites (`snapz_codec_test`,
`region_test`, `protocol_test`, `control_test`, `worldedit_test`, `eden_names_test`,
`eden_file_test`, `eden_import_test`, `matchmaker_test`). If any of them fails, stop — the
build is not usable.

The script uses `$CXX` if set, otherwise prefers `clang++` and falls back to `g++`.

Two live tests are **not** run by the build, because they bind a port and spawn server
processes. Run them by hand — the second one especially, before you host anything publicly:

```bash
python3 phase1_live_test.py    # join order, PONG, SIGNQ, the connection limits
python3 phase3_live_test.py    # attacks the command surfaces; takes about 2 minutes
```

> ⚠️ The resulting binary is native to the machine that built it. Do not copy an arm64 build
> (Apple Silicon, ARM VPS) to an x86-64 host or vice versa. Build on the box you run on.

## 3. Pick a world directory

The server reads and writes `eden_world.model`, `eden_players.txt` and `eden_signs.txt`
**relative to its working directory**. The tidiest setup is one directory per world.

Two sample worlds ship in `worlds/`:

| Directory | Contents |
|---|---|
| `worlds/ari/` | a built-up world (~3000 edited cells), no signs |
| `worlds/phase1test/` | a smaller world with three example signs |

Copy one to start from it, or make an empty directory for a fresh world — the server creates
the files on its first save.

```bash
cp -r worlds/ari ~/edenworlds/myworld
```

### Starting from an existing world

If you already have a saved `.eden` world, convert it instead of starting from a sample:

```bash
./eden_import ~/Downloads/myworld.eden        # writes worlds/myworld/
./eden_import ~/Downloads/myworld.eden --dry-run   # or just see what it would cost
```

That directory is ready to host: terrain (caves included), signs and a spawn point. Read
[import.md](import.md) before importing anything large — it explains the two size numbers the
tool prints, and why the second one is the one that decides whether the world plays.

## 4. Run

```bash
cd ~/edenworlds/myworld
/path/to/ewb-server/edenserver
```

```
========================================
  Eden Multiplayer Server
  Listening on port 27015
========================================
```

Useful variations:

```bash
edenserver --port 27016                  # a different port
edenserver --password hunter2            # require a password at JOIN
edenserver --verbose                     # log every edit and every rejection
```

`host_world.sh` wraps this for the common case, but note that it always passes a matchmaker
address (defaulting to `127.0.0.1`); see [configuration.md § host_world.sh](configuration.md#host_worldsh).

Stop it with Ctrl-C. The world is saved every 15 seconds and whenever a player disconnects, so
at worst you lose 15 seconds of building.

Open the port for other players: **TCP 27015** (or whatever `--port` you chose) through your
firewall, and forwarded on your router if they are off your LAN.

## 5. Connect a retail client

The retail game has **no "connect to IP" field** — the shipped flow is the in-game Server
Browser, which lists servers from a first-party matchmaker. To reach a server you host, use the
client's direct-connect environment variable, `EDEN_MP_AUTOJOIN`, set to `host:port`:

```bash
# Linux / a shell-launched client
EDEN_MP_AUTOJOIN=192.168.1.20:27015 <path-to-the-game-executable>
```

On macOS a wrapped app bundle does not inherit a terminal's environment, so set it for the
whole login session first:

```bash
launchctl setenv EDEN_MP_AUTOJOIN "192.168.1.20:27015"
# launch the game normally, then afterwards:
launchctl unsetenv EDEN_MP_AUTOJOIN
```

Use `127.0.0.1:27015` if the client and server are on the same machine.

If it worked, the server logs a connection and a join, and the client drops straight into the
world instead of the browser:

```
[Server] Client #1 connected from 127.0.0.1
[Server] Player6835 (Type 0) has joined.
[Server] REGION #1 Player6835 (65536,65536) ... 1169 cells -> ... 1 frame(s) ...
```

Your position is remembered by username, so rejoining puts you back where you left off.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Client connects, world is empty flat terrain | Expected if the world has no edits — only edits are stored and sent. |
| Client connects but the world never appears | The client did not get a `REGION` answered. Run with `--verbose` and check for a `REGION` line; if none arrives, the client never sent one. |
| `[Server] Invalid name (…)` | The username breaks a rule — see [protocol.md § Usernames](protocol.md#usernames). |
| `[Server] Name already in use.` | That username is already connected. Names are unique per server. |
| `[Server] Server full.` | 64 clients already connected. |
| `[Server] Rejected <ip> (connect rate limit).` | More than 10 connections from one address in 10 s. Raise or disable with `--connect-limit`. |
| `[Server] Rejected <ip> (auth lockout).` | That address sent too many wrong passwords and is in an escalating cooldown. Tune or disable with `--auth-fail-limit`. |
| `[Server] client #N (<ip>) sent no JOIN within 15s; dropping.` | A connection opened but never sent `JOIN` inside the handshake window. Usually a port scanner. Tune with `--handshake-timeout`. |
| Bind fails on startup | Another process holds the port. |
| Edits vanish on rejoin | The per-connection edit budget dropped them. Raise `--action-rate` / `--action-burst`. |

## What this does not cover

These are real gaps, not omissions from this page. Detailed status lives in the project's
development notes (`WORKING/ROADMAP-SERVER.md`), which are local-only and not published.

- **Running as a system service** — systemd unit, dedicated user, firewall, backups, log
  rotation. That is **Phase 4**. Its artifacts already exist in `ops/` (`edenserver.service`,
  `edenserverctl`, `INSTALL.md`) but have not been verified end-to-end on a Linux host; a
  `docs/operations.md` will land here when the phase closes.
- **Appearing in the *first-party* in-game Server Browser** — the retail client reads its
  server list from a matchmaker at a fixed address, and it exposes no way to point it elsewhere
  short of a build change (the same limitation as direct-connect). So a self-hosted server
  cannot show up in the *retail* browser today; `EDEN_MP_AUTOJOIN` remains the way in.
  The matchmaker *protocol* itself is implemented, both sides: `edenserver --matchmaker` plus
  the standalone `edenmatch` server (see [matchmaker.md](matchmaker.md)). That is a working
  Server-Browser listing for **LAN play, testing, and private friends-lists** — any setup where
  you also control which matchmaker the clients talk to.
- **Block and colour *names* in commands** — `//set 2` works, `//set stone` does not. The
  numeric ids are the wire's own (`0..127` blocks, `0..54` colours); the community name tables
  are a later roadmap stage. Everything else in the player command set is built: `//set`,
  `//sphere`, `//copy`/`//paste`, `//undo`, `/tp`, `/msg`, and permission levels — see
  [commands.md § Part 2](commands.md#part-2--player-commands). Ramp *facing* under `//rotate` is
  also not yet verified.
  `server_posix_modded.cpp` in this repository is the community patch the vocabulary came from.
  It is **unauthenticated and unsafe to host** — kept for its data tables, not to be run.
- **Writing signs from the game client** — the wire protocol has no known client sign-write
  message. Signs are authored in `eden_signs.txt`, by hand or with `edenctl signs add`.
