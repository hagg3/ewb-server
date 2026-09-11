# Configuration reference

**Audience:** operators hosting a server, and anyone changing how it is configured.

**When you add, rename or remove a command-line flag, or change an on-disk file format, update
this file in the same commit.**

Everything the server can be told is a **command-line argument**. `edenserver` itself reads
**no environment variables at all**. The `EDEN_*` variables in
[`/etc/edenserver.conf`](#the-systemd-environmentfile-etcedenserverconf) are expanded by
*systemd* into that command line before the binary starts — the server never sees them. The
other environment variables listed here belong to the build script, the ops wrapper, and the
game client.

## Command-line arguments

```
./edenserver [port] [flags...]
```

A bare leading number is still accepted as the port, for compatibility with the original
server. Unknown flags are ignored.

### Identity and network

| Flag | Default | Meaning |
|---|---|---|
| `--port N` | `27015` | TCP port to listen on. Binds all interfaces. |
| `--name "Text"` | `Eden Server` | Server name sent to a matchmaker. Has no effect without `--matchmaker`. |
| `--password PASS` | *(empty)* | If set, `JOIN` must supply a matching password; empty means an open server. |
| `--matchmaker HOST[:PORT]` | *(none)* | Register with a matchmaker at this address and hold the registration open (`REGISTER` → `REGISTERED`, then a bare `PING` keep-alive every 20 s). Port defaults to `27020`. Reconnects every ~5 s if the matchmaker is down. See [matchmaker.md](matchmaker.md). |
| `--advertise IP` | auto-detected | The address clients should use to reach this server, as told to the matchmaker. When omitted and a matchmaker is configured, the server picks this machine's primary non-loopback LAN IPv4 (preferring `192.168.*`, `10.*`, `172.*`) so remote devices do not get handed `127.0.0.1`. |

### Files

| Flag | Default | Meaning |
|---|---|---|
| `--world FILE` | `eden_world.model` | World edit store. Created on first save if absent. |
| `--signs FILE` | `eden_signs.txt` | Sign sidecar. Read at startup; the control socket's `signs add`/`rm`/`reload` also edit it. Absent is normal and silent. |
| `--spawn-file FILE` | `eden_spawn.txt` beside `--world` | World default-spawn sidecar (one line `x:y:z`), as written by [`eden_import`](import.md). Read once at startup. Absent is normal and silent; a malformed line warns and is ignored. |
| `--spawn x:y:z` | *(none)* | Set the world default spawn inline; overrides `--spawn-file` and skips reading it. |

Player positions are always read from and written to `eden_players.txt` — there is no flag for
it. All paths are resolved **relative to the process working directory**, so run the
server from the world's directory (or pass absolute paths).

The world default spawn is where a joining player who has **no `eden_players.txt` row of their
own** is placed (`SPAWN` unicast). A returning player's saved position always wins; with neither,
the client decides.

### Lifecycle

| Flag | Default | Meaning |
|---|---|---|
| `--idle-timeout N` | `0` (off) | Exit cleanly after `N` seconds with zero connected clients. Checked on the 15 s autosave tick, so the real granularity is 15 s. ⚠️ Do not combine with a service supervisor set to restart unconditionally — the idle exit is a normal `exit(0)` and would become a start/idle/exit loop. |
| `--handshake-timeout N` | `15` | Seconds a freshly accepted connection has to send its `JOIN` before the server drops it. Stops a peer from opening TCP connections that send nothing and holding client slots until the ~2 h TCP keepalive reaps them. `0` disables (not recommended on an internet-facing port). |
| `--idle-timeout-conn N` | `300` | Seconds of total silence tolerated on a socket *after* `JOIN` before it is dropped. The retail client sends `PING` every 10 s and `POS` while moving, so a genuinely silent joined socket is dead. `0` disables. Distinct from the world-level `--idle-timeout` above. |

### Operator control socket

The server opens a **Tier 1 operator control socket** — a `0600` unix domain socket that lets
whoever can open it run `kick` / `ban` / `save` / `fill` / `stop` and the rest. It never
touches the client wire. Full command list and the `edenctl` client: [commands.md](commands.md).

| Flag | Default | Meaning |
|---|---|---|
| `--control-socket PATH` | `<world dir>/edenserver.sock` | Path for the control socket. The default sits next to `--world`. |
| `--no-control-socket` | socket on | Do not open the control socket at all. |
| `--ban-file FILE` | `eden_bans.txt` | Persisted ban list (names and IPs). Checked at `accept()` for IPs and at `JOIN` for names. |
| `--ops-file FILE` | `eden_ops.txt` | Persisted per-player op levels (`0` visitor / `1` builder / `2` operator). Read by the Tier 2 player commands below. |
| `--default-level N` | `0` | Level for a player with no `eden_ops.txt` entry. Clamped to `0..2`. **The single most important setting on a public server** — `1` hands WorldEdit to every visitor. |
| `--control-rate N` | `16` | Control commands per second one connection may issue. `0` disables the pacing (useful while driving a bulk import through `edenctl`); the concurrent-connection cap below still applies. |
| `--control-burst N` | `64` | Control commands one connection may issue at once before the sustained rate applies. Clamped to at least `1`. |
| `--control-max-conns N` | `8` | Concurrent control connections. Each one costs a thread; a further connection is answered `error: too many control connections` and closed. Clamped to at least `1` — there is no "unlimited". |

Filesystem permissions decide **who** may open the control socket; the three flags above decide
**how fast**. They are not a security boundary — they are what stops an operator's own runaway
script from holding the world lock, which before they existed was indistinguishable from an
attack. A connection that keeps sending after being throttled is disconnected after 8 refusals,
and one that sits silent for 300 s is closed so it cannot hold a slot.

Filesystem permissions on the socket **are** the authentication — there is no password and
nothing to point at a network address. Keep it inside the service user's directory. Under
systemd that is automatic: `ops/edenserver.service` passes
`--control-socket ${EDEN_WORLD_DIR}/edenserver.sock`, so the socket always sits in the active
world directory alongside the world files.

### Player commands

The **Tier 2 player command surface** — the `//set` / `//sphere` / `/tp` verbs players type into
the game's chat box. Full reference: [commands.md § Part 2](commands.md#part-2--player-commands).
Unlike the operator socket, everything here is driven by untrusted input, so the bounds below
are load-bearing rather than convenience.

| Flag | Default | Meaning |
|---|---|---|
| `--no-worldedit` | commands on | Disable every in-chat command. Chat still works; a `/`-prefixed line gets a one-line refusal. |
| `--we-max-cells N` | `131072` | Largest box a single command may read or write. Clamped to `1..--max-world-cells` (the world's edited-cell ceiling). A selection or radius larger than this is refused with its size, never silently truncated. **Also sets the operator `fill` cap**, at twice this value — both tiers bound the same cost (one pass over the world holding the world lock), so there is one number for it. |
| `--we-rate N` | `32768` | Cells per second a player may spend. `0` disables the budget — the per-command cap in `--we-max-cells` still applies. |
| `--we-burst N` | `262144` | Cells a player may spend at once before the sustained rate applies. Raised automatically to at least `--we-max-cells`, so one full-size command always fits. |
| `--we-undo-budget N` | `2097152` | Bytes of undo **and** redo history per player, oldest batch evicted first. Raised automatically to hold at least one full `--we-max-cells` batch, so undo never discards the edit you just made. |

The 64-player worst case for undo memory is `--we-undo-budget` × 64 — about 128 MiB at the
default. Lower it on a small VPS; raise it if your builders complain that undo runs out.

The rate limit is charged in **cells, not commands**, and charged against the box a command will
scan before any of it runs — one `//sphere 40 2` is a single chat line and roughly 268,000 cells.

### Logging

| Flag | Default | Meaning |
|---|---|---|
| `--verbose` | off | Log every terrain edit, every throttled or rejected request, and the first sighting of each unrecognised verb. Chatty on a busy server. |
| `--audit-file FILE` | none | Keep a second copy of the audit channel in `FILE`, appended to. stdout always gets it; this survives independently of the journal. |

One line per served `REGION` is logged regardless of `--verbose` (cells scanned, records,
frames, wire bytes, compression ratio, scan and encode milliseconds) — it is the measurement
that tells you whether region serving is keeping up.

#### The audit channel

**Every change to server state is logged unconditionally**, whoever made it and whatever
`--verbose` is set to. One line, UTC-timestamped, prefixed `[Audit]`:

```
[Audit] 2026-09-09T14:03:11Z control fill 4096 cells = 2 @ 65530,32,65530..65545,47,65545
[Audit] 2026-09-09T14:03:19Z player:hagge //set: 512 cell(s)
[Audit] 2026-09-09T14:04:02Z player:hagge (level 2) /tp Someone
```

- `control …` — the operator socket: `say`, `kick`, `ban`, `unban`, `op`, `deop`, `save`,
  `stop`, `setblock`, `fill`, `signs add|rm|reload`.
- `player:<name> …` — an in-chat command that **changed cells**, with the count it actually
  changed, at every permission level. Plus any command that reaches across players (today only
  `/tp <player>`, which is audited on the way in because it edits nothing).

Read-only commands (`who`, `banlist`, `region-stats`, `//pos1`, `/help`) are not audited: the
channel is only useful if everything in it is a change. Refused commands are logged only under
`--verbose` — a player can trigger a refusal far faster than they can trigger an edit, so
auditing them would let anyone who can chat fill your disk.

`grep '\[Audit\]'` over the journal is the intended way to read it; `--audit-file` is for
keeping a copy that outlives `journalctl --vacuum`.

### Limits

| Flag | Default | Meaning |
|---|---|---|
| `--max-world-cells N` | `4000000` | Ceiling on distinct edited world cells held in memory (and written to `eden_world.model`). At the cap, edits to cells the world already holds still apply but **every new cell is refused** — a block placed in open air, a natural block mined or painted — so from a player's seat some builds save and others vanish. **Size it above the world, never equal to it:** [`eden_import`](import.md)'s summary prints the value to use (the cell count plus a quarter, at least 1,000,000 more, rounded up to 100,000). The server warns at startup when the loaded world is at the cap or within a tenth of it, logs refusals at most once a minute (`world cell cap reached`), and tells the player whose edit was refused; every `Saved world` log line shows the cap. Also the upper clamp for `--we-max-cells` and the derived `fill` cap. Your RAM is the real limit. A value below `1` falls back to the default with a warning; a non-default value is logged at startup. |
| `--action-rate N` | `512` | Sustained terrain edits per second per connection. `0` disables the limit entirely. |
| `--action-burst N` | `1024` | Edits a connection may spend at once before the sustained rate applies. |
| `--connect-limit N` | `10` | New connections allowed per source IP per 10 s window. `0` disables. ⚠️ It is per *source address*, so a whole LAN behind one NAT address shares the allowance — as does a test harness on loopback. |
| `--auth-fail-limit N` | `5` | Wrong-password `JOIN` attempts allowed per source IP inside a 60 s window before that IP is locked out at `accept()`. The lockout starts at 60 s and **doubles** on every further failure, up to 1 h; an IP that stops guessing for an hour has its escalation reset. `0` disables. Per-IP only — a distributed guesser is not stopped by this (see below). |

The defaults accommodate bulk editing by a scripted client draining a queue at several hundred
edits per second. Tighten them if you host strangers. A refused edit is dropped, not fatal.
Burn costs 64 against the budget rather than 1, because a single burn can write hundreds of
cells via explosion simulation.

The password compare at `JOIN` is constant-time. `--auth-fail-limit` throttles a **single-IP**
brute force; a distributed guesser (many IPs, few tries each) still slips through it. If you
rely on a password to gate strangers, also watch the journal for `wrong password` lines with
fail2ban (a ready-made filter/jail ships in [`ops/`](../ops/)) and prefer the ban list or an
allow-list over a shared secret where you can.

### Region tuning

These exist for protocol experimentation. Leave them alone for normal hosting.

| Flag | Default | Meaning |
|---|---|---|
| `--region-radius N` | `224` | Blocks a `REGION` reply covers around the request point. Clamped to `16..4096`; out-of-range values fall back to 224 with a warning. A non-default value is logged at startup, because client-side coverage patterns assume 224. |
| `--no-region-sort` | sorting on | Skip sorting records before deflate. Sorting costs a little CPU and buys a materially better compression ratio. |
| `--no-region-empty-frame` | frame on | Answer an empty region with silence instead of `SNAPZ:0:`. For A/B comparison against the retail server only; silence can leave a client waiting forever. |
| `--region-empty-frame` | — | Accepted and does nothing; the behaviour it names is now the default. |

### Compatibility

| Flag | Default | Meaning |
|---|---|---|
| `--legacy-snapshot` | off | Push the whole world to a joining client as plaintext `ACTION` lines, instead of waiting for `REGION`. See [protocol.md § Legacy world snapshot](protocol.md#legacy-world-snapshot). Bring-up fallback; not what a retail client expects. |

### Compiled-in limits

Not configurable at runtime; listed so you know what the server will do under load. Change them
in `server_posix.cpp` (and its headers) if you must.

| Limit | Value |
|---|---|
| Max concurrent clients | 64 (further connections get `[Server] Server full.`) |
| Max bytes buffered without a newline | 8192 |
| Max chat message length | 256 bytes |
| Max username length | 20 bytes |
| Max distinct edited world cells | 4,000,000 by default — the value of `--max-world-cells` (see the server flag table) |
| Max signs (loaded, or placed by players) | 20,000 |
| Sign writes (`SIGNP`) per connection | 1/s sustained, burst of 8 |
| Minimum gap between served `REGION`s | 750 ms |
| `REGION`s served per session | 256 |
| Minimum gap between served `SIGNQ`s | 1000 ms |
| `SIGNQ`s served per session | 32 |
| Autosave interval | 15 s |
| World height (`y`) | 0–255 |
| Horizontal coordinate range (`x`, `z`) | 0–16777215 |
| Accepted block types (build) | 0–127 |
| Accepted paint indices | 0–54 |
| Max cells in one control-socket `fill` | 262,144 (`2 x --we-max-cells`, capped at the world ceiling) |
| Control-socket commands per connection | 64 at once, 16/s sustained |
| Control-socket refusals before disconnect | 8 |
| Concurrent control connections | 8 |
| Control-socket idle timeout | 300 s |
| Max radius/height argument to a player shape command | 512 |
| Block a player `//up` stands on | 58 |
| Failed-auth counting window | 60 s |
| Auth lockout: base / max / reset-after | 60 s / 1 h (doubling) / 1 h idle |

## The systemd EnvironmentFile (`/etc/edenserver.conf`)

`ops/edenserver.service` reads its settings from `/etc/edenserver.conf` via
`EnvironmentFile=-/etc/edenserver.conf`. Copy the template and edit it:

```sh
sudo install -m 0600 -o root -g root ops/edenserver.conf.example /etc/edenserver.conf
sudoedit /etc/edenserver.conf
sudo systemctl restart edenserver          # settings apply on restart
```

The leading `-` means a missing file is not a startup failure — but then the required keys
expand to empty and the server exits with a flag error in the journal, so create the file. Only
`ops/edenserver.service` uses this; `./edenserver` run by hand still takes plain flags, and
`host_world.sh` is unchanged.

### Keys

| Key | In `ExecStart` as | Default (template) | Maps to |
|---|---|---|---|
| `EDEN_PORT` | `--port ${EDEN_PORT}` | `27015` | `--port` |
| `EDEN_NAME` | `--name ${EDEN_NAME}` | `Eden Server` | `--name` |
| `EDEN_WORLD_DIR` | `--world ${EDEN_WORLD_DIR}/eden_world.model` and the sign / spawn / ban / ops / control-socket paths | `/var/lib/edenserver/world` | the active world directory |
| `EDEN_MAX_WORLD_CELLS` | `--max-world-cells ${EDEN_MAX_WORLD_CELLS}` | `4000000` | `--max-world-cells` |
| `EDEN_PASSWORD` | `--password ${EDEN_PASSWORD}` | *(empty)* | `--password`; empty is identical to omitting it (open server) |
| `EDEN_EXTRA_ARGS` | a bare `$EDEN_EXTRA_ARGS` tail | *(absent)* | any spaceless optional flags: `--matchmaker HOST:PORT`, `--spawn x:y:z`, `--audit-file PATH`, `--default-level N`, … |

### The `${VAR}` vs `$VAR` rule

systemd expands the two forms differently, and getting it wrong is a real bug:

- **`${VAR}` (braced)** → exactly **one** argument, spaces preserved — *even when the value is
  empty*, in which case it passes an empty string argument. Used for the five keys above, each
  of which the template always sets.
- **`$VAR` (bare)** → split on whitespace into **zero or more** arguments, no quote handling.
  Used once, for `EDEN_EXTRA_ARGS`, which therefore vanishes cleanly when the key is empty or
  absent.

So a setting that is *optional* **and** can contain a space cannot be its own `${VAR}` — an
unset `--matchmaker ${EDEN_MATCHMAKER}` would dial the empty host forever. Those go in
`EDEN_EXTRA_ARGS`. Hostnames, ports and coordinates have no spaces and are fine there.
`edenadmin`'s **Config** panel writes an empty `EDEN_EXTRA_ARGS` by *omitting the line*, for
this reason.

### `ops/edenserver-writeconf`

The only path with write access to `/etc/edenserver.conf` that `edenadmin`'s sudoers rule
grants (never `sudo tee`). It reads the proposed file on stdin, rejects it unless every
non-blank / non-comment line is `EDEN_<KEY>=<value>`, checks the five required keys and that
`EDEN_PORT` / `EDEN_MAX_WORLD_CELLS` are positive integers and `EDEN_WORLD_DIR` is absolute,
then installs it atomically (temp file + `rename(2)`) at `0600 root:root`. Target path is
`/etc/edenserver.conf` or `$EDENSERVER_CONF` if set.

## `host_world.sh`

A convenience launcher. It builds `edenserver` first if it is missing, then execs it.

```
./host_world.sh <worldFile> [name] [port] [password] [matchmakerHost]
```

| Position | Default | Maps to |
|---|---|---|
| 1 `worldFile` | `eden_world.edits` | `--world` |
| 2 `name` | `Eden Server` | `--name` |
| 3 `port` | `27015` | `--port` |
| 4 `password` | *(none)* | `--password`, omitted if empty |
| 5 `matchmakerHost` | `127.0.0.1` | `--matchmaker` |

If an `eden_signs.txt` sits in the same directory as `worldFile`, it is passed as `--signs`.
That is what makes a world written by `eden_import` — which puts every file in its own
directory — load its signs without a second flag. `eden_spawn.txt` in that directory is picked
up by the server itself, so it needs no flag here either.

Any arguments after the fifth position are forwarded to `edenserver` verbatim, e.g.
`./host_world.sh w "N" 27015 "" 127.0.0.1 --max-world-cells 8000000`.

⚠️ It **always** passes `--matchmaker`, defaulting to `127.0.0.1`. With no matchmaker running
there, the server retries the registration in the background every few seconds; it serves
clients normally regardless. Run `./edenserver` directly if you want no matchmaker traffic at
all.

## `eden_import`

The offline `.eden` → server-world converter, built alongside `edenserver` by
`build_server.sh`. Full walkthrough, the base terrain profile and how to read the two budget
numbers are in [import.md](import.md); the flags:

```
./eden_import <world.eden> [flags...]
```

| Flag | Default | Meaning |
|---|---|---|
| `--name NAME` | a slug of the world's own name | Output directory name under `worlds/`. |
| `--out DIR` | `worlds/<name>` | Write here instead. |
| `--force` | off | Overwrite an existing output directory. |
| `--dry-run` | off | Project and print the summary; write nothing. |
| `-y`, `--yes` | off | Take every prompt's default; no interactive questions. Implies `--force`. This plus the flags below is the scripting path. |
| `--air-fill diff\|solid\|full` | `diff` | Which voxels become cells. `diff` emits only what differs from the client's base terrain; `solid` drops air (sub-surface voids fill in); `full` emits every voxel. |
| `--base-profile default\|none\|FILE` | `default` | The terrain `diff` compares against. Ignored by the other two strategies. |
| `--signs FILE` | `signs_<input>.dat` beside the input | The sign sidecar. Takes precedence over the world's inline sign trailer. |
| `--no-signs` | off | Ignore signs entirely. |
| `--spawn header\|home\|X,Y,Z\|none` | `header` | What goes into `eden_spawn.txt`. |
| `--max-world-cells N` | `4000000` | Refuse above this many cells. Matches the server's `--max-world-cells` default. The summary's `server cap` line is the value to start `edenserver` with — **above** the import's cell count, so players have room to build. An import that fits under this flag but would leave less room than that gets a warning. |
| `--max-region-records N` | `2000000` | Refuse if any single `REGION` reply would carry more records than this. `0` disables the check. |
| `--region-radius N` | `224` | Match a server started with `--region-radius`; changes the size of the box the projection slides. |
| `--strict` | off | Treat unknown block ids and out-of-palette paints as errors instead of warnings. |
| `-h`, `--help` | — | Usage. |

Run on a terminal with no options pinned, `eden_import` walks through the strategy (with the
projected cell and `REGION` numbers for each), the spawn source, the output name and the
overwrite confirmation. Each prompt is skipped when the matching flag is given; a pipe or
`--yes` skips all of them and takes the defaults. See [import.md](import.md#interactive-session).

A block type of `255` is always a hard error: it collides with the server's painted-base
sentinel, so those cells would be silently dropped from every `REGION` reply.

The base-profile file grammar is one layer per line, `#` comments and blanks skipped;
unmentioned heights are air:

```
# height[-height] : type [: paint]
0:1
1-15:2
16-31:3
32:8
```

## `edenmatch`

The standalone matchmaker, built alongside `edenserver` by `build_server.sh`. Full protocol and
usage in [matchmaker.md](matchmaker.md); the flags:

```
./edenmatch [port] [flags...]
```

| Flag | Default | Meaning |
|---|---|---|
| `--port N` | `27020` | TCP port to listen on (all interfaces). A bare leading number also works. |
| `--advertise-ip IP` | *(peer address)* | Override the advertised address for every registration. |
| `--short-list` | off | Emit the 4-field `SERVER:` row instead of the 7-field capture grammar. |
| `--verbose` | off | Log every `LIST` / `HOST` / bad line, not just registrations. |
| `--allow-host` | off | Honour client `HOST:` requests by spawning a local `edenserver`. |
| `--edenserver PATH` | `./edenserver` | Binary to spawn for `HOST`. |
| `--world DIR` | `.` | Working directory for a spawned server. |
| `--host-ports LO-HI` | `27600-27699` | Port range `HOST` allocates from. |

`edenmatch` holds all state in memory — no config file, nothing on disk.

## `edenctl`

Client for the operator control socket. See [commands.md](commands.md) for the command
reference; the short version:

```
./edenctl [-S <socket>] <command> [args...]
```

With no `-S`, it looks for the socket at `$EDENSERVER_CONTROL_SOCKET`, then `./edenserver.sock`,
then `$EDENSERVER_WORLD_DIR/edenserver.sock`, then `/var/lib/edenserver/world/edenserver.sock`.
It needs one of `nc` (with `-U`), `socat`, or `python3` on the host.

| Variable | Default |
|---|---|
| `EDENSERVER_CONTROL_SOCKET` | *(unset — falls through to the paths above)* |

## `run_server.bat`

Windows launcher for the **MSVC build of the original Winsock server** (`server.cpp` →
`TCPServer.exe`), kept as reference. It searches the usual MSVC output directories and takes no
arguments. It does **not** launch `edenserver`, which is the POSIX build. On Windows, use WSL.

## Environment

### Build

| Variable | Used by | Meaning |
|---|---|---|
| `CXX` | `build_server.sh` | Compiler to use. If unset the script prefers `clang++`, then `g++`, and errors out with the package names to install if neither exists. |

### Ops wrapper

`ops/edenserverctl` — `start` / `stop` / `restart` / `status [--porcelain]` / `logs [N]` /
`logs-tail [N]` / `backup` — is a thin `systemctl` / `journalctl` wrapper. It reads:

| Variable | Default |
|---|---|
| `EDENSERVER_SERVICE` | `edenserver` |
| `EDENSERVER_WORLD_DIR` | `$EDEN_WORLD_DIR`, else `/var/lib/edenserver/world` |
| `EDENSERVER_BACKUP_DIR` | `$EDEN_BACKUP_DIR`, else `/var/lib/edenserver/backups` |
| `EDENSERVER_BACKUP_COMPRESS` | `$EDEN_BACKUP_COMPRESS`, else `1` (gzip backups; `0` = plain copies) |
| `EDENSERVER_BACKUP_LEVEL` | `$EDEN_BACKUP_LEVEL`, else `6` (gzip level, 1–9) |

The `EDEN_*` forms are honoured as a fallback so
`ops/edenserver-backup.{service,timer}` can back up whichever world is active just by sourcing
`/etc/edenserver.conf`.

- `status --porcelain` prints machine-readable `Key=Value` lines (`ActiveState`, `SubState`,
  `MainPID`, `ExecMainStartTimestamp`, `Result`) with no pager and no follow.
- `logs-tail [N]` is `journalctl -u <svc> -n N --no-pager` with **no** `-f` — the scriptable
  counterpart to `logs`, which follows forever.
- `backup` runs `edenctl save` first (best effort) then **gzips** the world files —
  `eden_world.model`, `eden_players.txt`, `eden_signs.txt`, `eden_spawn.txt`, whichever exist —
  into `$EDENSERVER_BACKUP_DIR/<UTC stamp>/` as `<name>.gz`. A world file is one plaintext
  `x:y:z:type:color` line per edited cell and deflates to roughly a sixth of its size (a 40 MB
  world → ~7 MB); nothing reads a backup directly, so they are compressed by default. Level 6 is
  the default because 9 costs several times the CPU for a few percent. Each archive is written to
  a sibling `.tmp` and `mv`'d into place, so an interrupted backup leaves no truncated file, and
  the archive keeps the source's mtime. Restore with
  `gunzip -c <backup>/eden_world.model.gz > <world dir>/eden_world.model` while the server is
  stopped. `EDENSERVER_BACKUP_COMPRESS=0` restores the old plain-`cp` behaviour; if `gzip` is
  missing the command warns and copies plain rather than failing.
  `ops/edenserver-backup.timer` (`OnCalendar=hourly`, `Persistent=true`) runs it on a schedule;
  install with `sudo systemctl enable --now edenserver-backup.timer`.

### Client side

| Variable | Read by | Meaning |
|---|---|---|
| `EDEN_MP_AUTOJOIN` | the **game client**, not this server | `host:port`. Makes the retail client connect directly to that address on launch, bypassing the in-game Server Browser. This is how you join a self-hosted world today. |

On macOS a wrapped app bundle does not inherit a shell's environment, so set it for the login
session (`launchctl setenv EDEN_MP_AUTOJOIN "127.0.0.1:27015"`) before launching the game, and
unset it afterwards.

## File formats

All of them are plain text, LF-terminated, and safe to hand-edit while the server is stopped.
`eden_world.model` and `eden_players.txt` are rewritten by the server via a temp file plus
`rename()`, so an edit made while it is running will be overwritten at the next save.

### `eden_world.model`

One edited cell per line. The base terrain is generated identically on every client and is not
stored — only the differences are.

```
x:y:z:type:color
```

| Field | Range | Meaning |
|---|---|---|
| `x`, `z` | `0 .. 16777215` | Horizontal position, centred on 65536. |
| `y` | `0 .. 255` | Height. |
| `type` | `0`, `1..254`, `255` | `0` = air (mined). `1..254` = a placed block of that type. `255` = a natural block that was only painted. |
| `color` | `0 .. 54` | Paint index; `0` = unpainted. |

A line with fewer than four fields is skipped; a missing fifth field is read as `0`. Fields are
parsed leniently (non-numeric text becomes `0`), so a malformed file degrades into wrong blocks
rather than a failure to start. Order is not significant, and the server writes them in hash
order — do not expect a stable diff between saves.

```
65521:31:65552:0:0      # mined cell (air) near the origin
65544:34:65558:2:0      # block type 2 placed
```

### `eden_players.txt`

One line per remembered player. Used to send `SPAWN` on rejoin.

```
username:x:y:z
```

Coordinates are floats. `username` cannot contain `:` (the protocol forbids it), so the first
`:` always ends the name. A line without a `:`, or whose three coordinates do not all parse, is
skipped.

```
Player6835:65535.2:33.92:65545.2
```

### `eden_signs.txt`

Written by operators and players. Parsed once at startup. A sign a player places or edits in
game is saved to it on the next autosave, when they disconnect, or on the control socket's
`save`/`stop`; the control socket's `signs add` / `signs rm` rewrite it immediately and
`signs reload` re-reads it ([commands.md](commands.md)). Every write goes through a temp file +
`rename()`, like the world file.

**Don't hand-edit it while the server is running** — the next save rewrites the file from
memory and your edit is lost. Stop the server, edit, start it (or use `signs add`/`rm`).

```
x:y:z:a:b:c:text
```

- Exactly six `:` are consumed; **everything after the sixth is the text**, so a `:` inside
  sign text is safe.
- Blank lines and lines beginning with `#` are skipped silently.
- A malformed line is ignored with a warning (the first five are reported individually).
- `x`, `z` must be `0..16777215`; `y` must be `0..255`. Every one of the six numeric fields
  must be a plain integer with no trailing junk.
- Text is capped at 256 bytes and stripped of ASCII control characters.
- ⚠️ `a`, `b`, `c` are of unknown meaning. They are emitted to clients verbatim. `a` has only
  been seen as `0..5` (likely a block face), and a player's edit replaces the sign with the same
  `x:y:z:a`. `0:0:0` is a reasonable default.

```
# format: x:y:z:a:b:c:text   (a/b/c semantics unknown, emitted verbatim)
65516:34:65536:0:0:0:PHASE1 SIGN WEST
```

### `eden_bans.txt`

Written and read by the control socket's `ban` / `unban` / `banlist`. One token per line;
blank lines and `#` comments ignored. A token made only of digits, dots and colons is treated
as an IP (matched against the peer address at `accept()`); anything else is an exact-match
username (checked at `JOIN`). Safe to hand-edit while the server is stopped.

```
# edenserver ban list — one name or IP per line
203.0.113.9
Griefer
```

### `eden_ops.txt`

Written and read by `op` / `deop`. One `name:level` per line, `level` in `0..2`. The name is
split on its **last** `:`, so a name containing `:` still round-trips. The file is read at
startup, and the level is consulted on **every** player command — an `op` or `deop` takes effect
on that player's next line, with no reconnect. A name with no entry gets `--default-level`.

```
# edenserver op levels — <name>:<0..2> per line
Alice:2
```

### `eden_spawn.txt`

A world's default spawn point, one line, written by [`eden_import`](import.md):

```
x:y:z
```

Coordinates are floats in server order — `x` and `z` horizontal, `y` height.

Read once at startup, from `eden_spawn.txt` beside the world file (or `--spawn-file`), and
overridden by `--spawn x:y:z`. A joining player with **no `eden_players.txt` row** is sent here
with a `SPAWN` unicast; a returning player keeps their saved position. A missing file is silent;
a malformed line prints one warning and is ignored (never fatal). Hand-edit friendly: `#`
comments and blank lines are skipped, the first valid line wins.

### `worlds/<name>/`

The layout `eden_import` writes, and the one the server expects when a world lives in its own
directory:

```
worlds/<name>/
  eden_world.model   the world            (--world)
  eden_signs.txt     signs                (--signs; host_world.sh derives it)
  eden_spawn.txt     default spawn        (--spawn-file; served on join)
  .gitignore         `*` plus `!.gitignore`
  edenserver.sock    the control socket, created at run time
```

The `.gitignore` matters: `worlds/` is tracked in this repository, so without it an imported
world would be committed by a `git add -A`.

### Temporary files

Saves write `<file>.tmp` next to the real file and rename over it. Seeing one linger means a
save failed — the server logs why, and retries at the next interval.
