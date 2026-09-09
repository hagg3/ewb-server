# Configuration reference

**Audience:** operators hosting a server, and anyone changing how it is configured.

**When you add, rename or remove a command-line flag, or change an on-disk file format, update
this file in the same commit.**

Everything the server can be told is a **command-line argument**. `edenserver` itself reads
**no environment variables at all**; the environment variables listed below belong to the build
script, the ops wrapper, and the game client.

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

Player positions are always read from and written to `eden_players.txt` — there is no flag for
it. All three paths are resolved **relative to the process working directory**, so run the
server from the world's directory (or pass absolute paths).

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
systemd that is automatic; the unit's `WorkingDirectory` is the world directory.

### Player commands

The **Tier 2 player command surface** — the `//set` / `//sphere` / `/tp` verbs players type into
the game's chat box. Full reference: [commands.md § Part 2](commands.md#part-2--player-commands).
Unlike the operator socket, everything here is driven by untrusted input, so the bounds below
are load-bearing rather than convenience.

| Flag | Default | Meaning |
|---|---|---|
| `--no-worldedit` | commands on | Disable every in-chat command. Chat still works; a `/`-prefixed line gets a one-line refusal. |
| `--we-max-cells N` | `131072` | Largest box a single command may read or write. Clamped to `1..4000000` (the world's edited-cell ceiling). A selection or radius larger than this is refused with its size, never silently truncated. **Also sets the operator `fill` cap**, at twice this value — both tiers bound the same cost (one pass over the world holding the world lock), so there is one number for it. |
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
| Max distinct edited world cells | 4,000,000 |
| Max signs loaded | 20,000 |
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

⚠️ It **always** passes `--matchmaker`, defaulting to `127.0.0.1`. With no matchmaker running
there, the server retries the registration in the background every few seconds; it serves
clients normally regardless. Run `./edenserver` directly if you want no matchmaker traffic at
all.

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

`ops/edenserverctl` (a thin `systemctl`/`journalctl` wrapper with a `backup` verb) reads:

| Variable | Default |
|---|---|
| `EDENSERVER_SERVICE` | `edenserver` |
| `EDENSERVER_WORLD_DIR` | `/var/lib/edenserver/world` |
| `EDENSERVER_BACKUP_DIR` | `/var/lib/edenserver/backups` |

### Client side

| Variable | Read by | Meaning |
|---|---|---|
| `EDEN_MP_AUTOJOIN` | the **game client**, not this server | `host:port`. Makes the retail client connect directly to that address on launch, bypassing the in-game Server Browser. This is how you join a self-hosted world today. |

On macOS a wrapped app bundle does not inherit a shell's environment, so set it for the login
session (`launchctl setenv EDEN_MP_AUTOJOIN "127.0.0.1:27015"`) before launching the game, and
unset it afterwards.

## File formats

All three are plain text, LF-terminated, and safe to hand-edit while the server is stopped.
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

Operator-authored. Parsed once at startup; the control socket's `signs add` / `signs rm` /
`signs reload` ([commands.md](commands.md)) edit it at runtime (rewritten via temp file +
`rename()`, like the world file). Without the control socket it is effectively read-only —
restart to apply hand edits.

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
- ⚠️ `a`, `b`, `c` are of unknown meaning. They are emitted to clients verbatim. `0:0:0` is a
  reasonable default.

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

### Temporary files

Saves write `<file>.tmp` next to the real file and rename over it. Seeing one linger means a
save failed — the server logs why, and retries at the next interval.
