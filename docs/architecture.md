# Architecture

**Audience:** anyone changing the server — human or agent.

**When you change the join sequence, the threading model, the world model, persistence, or the
set of source files, update this file in the same commit.** Wire-format changes go in
[protocol.md](protocol.md); flags and file grammars go in [configuration.md](configuration.md).

## Shape of the codebase

The server is **one POSIX translation unit plus pure, header-only libraries**. There is no
build system beyond a shell script and no link step beyond `-lz` and `-pthread`.

```
server_posix.cpp     The server: sockets, threads, world model, persistence, dispatch.
snapz_codec.h        Raw DEFLATE + base64 + SNAPZ frame encoding.
region_query.h       REGION box geometry, Cell -> wire-record table, record ordering, framing.
sign_store.h         eden_signs.txt parsing + SIGNP formatting.
hardening.h          Username validation, ACTION payload validation, token bucket,
                     per-IP connect limiter, per-IP failed-auth limiter,
                     constant-time password compare, text sanitisation.
control.h            Operator control-socket line grammar + command table, the
                     eden_bans.txt / eden_ops.txt formats, fill volume + the cap it
                     derives from the player tier, the connection flood guard.
worldedit.h          Player command table with a permission level per row, chat-line
                     grammar, ~-relative coordinates, the selection cap, shape
                     predicates, the byte-bounded undo store, clipboard rotation.
matchmaker.h         Matchmaker REGISTER parsing, name sanitising, SERVER: row / LIST
                     formatting, the TTL registry. Used by edenmatch.cpp.
edenmatch.cpp        The standalone matchmaker: sockets, threads, on-demand HOST spawn.
                     A second single translation unit, same style as the server.
```

Each header has an offline test binary built and run by `build_server.sh`:

```
snapz_codec_test.cpp   SNAPZ round-trip (deflate/base64/frame).
region_test.cpp        Region box, cell encoding, frame splitting.
protocol_test.cpp      Signs, usernames, ACTION validation, rate limiters.
control_test.cpp       Control line grammar, command table, ban/ops files, fill bounds,
                       the derived fill cap, the flood guard's state machine.
worldedit_test.cpp     Player command table + permission floors, grammar, selection cap,
                       shape predicates, undo byte budget, clipboard rotation.
matchmaker_test.cpp    Name sanitising, REGISTER parsing, SERVER: row / LIST, the registry.
```

Supporting files: `build_server.sh` (build + run all suites), `host_world.sh`
(convenience launcher), `edenctl` (client for the operator control socket), `run_server.bat`
(Windows/MSVC launcher), the `phase1_*.py` and `phase3_live_test.py` scripts (run by hand,
not by the build — they bind a port and spawn processes), `worlds/<name>/` (sample worlds).

`phase3_live_test.py` is the adversarial counterpart to the offline suites: the suites prove the
command-surface bounds are correct, and it tries to break them over real sockets — oversized
selections against every command that reads one, permission escalation by casing and prefix
tricks, undo growth across a long session, command flooding, malformed-argument fuzzing, and the
control socket's own pacing and connection cap. Run it before hosting anything publicly.

`server.cpp` is the original Winsock server this was ported from — reference only, a strict
subset with no world model, persistence or validation. `server_posix_modded.cpp` is a
community in-chat-command patch, not built by default; its command surface is unauthenticated.
Do not develop against either.

### The header + offline test convention

**New protocol logic goes in a pure header with an offline `*_test.cpp` suite wired into
`build_server.sh`.** "Pure" means: no sockets, no globals the server owns, no clock of its own
— anything time-dependent takes `now` as a parameter (see `ewb::TokenBucket::allow`,
`ewb::ConnectLimiter::allow`). That is what lets a rate limiter be tested without sleeping and
an encoder be tested without a client.

`server_posix.cpp` supplies what the headers deliberately do not: the sockets, the locks, the
clock, and the policy constants.

Constants that must agree across headers are enforced with `static_assert` in
`server_posix.cpp` rather than by comment — for example the air and painted-base sentinels
shared by the model and the encoder, the paint-index ceiling shared by the ingest guard
(`hardening.h`) and the wire guard (`region_query.h`), and the sign `y` range against the world
height. If you change one side of such a pair, the build fails until you change the other.

## Threading model

| Thread | Created | Job |
|---|---|---|
| main | process start | `accept()` loop: IP ban check, auth lockout check, connect rate limit, client cap, spawn a handler |
| client handler | one per accepted socket, **detached** | the whole session: recv, line framing, dispatch, and its own cleanup |
| autosave | at startup, detached | every 15 s: `saveWorld()`, `savePlayerPos()`, and the `--idle-timeout` check |
| matchmaker | at startup **iff** `--matchmaker` was given, detached | keeps one TCP registration open, heartbeats a player count every ~15 s, reconnects on failure |
| control listener | at startup unless `--no-control-socket`, detached | `accept()` loop on the `0600` unix domain socket |
| control handler | one per control connection, **detached** | reads `\n`-framed command lines, runs each, replies; `stop` saves and exits the process |

Every thread is detached; nothing joins. A client handler removes itself from the roster and
closes its socket on the way out, so no handle accumulates. `SIGPIPE` is ignored at startup so
a peer that vanishes mid-write cannot take the process down. An exception escaping a detached
thread would `std::terminate` the whole server, so the paths that can throw (the SNAPZ
encoder) catch locally.

### Locks

| Mutex | Guards |
|---|---|
| `clientsMutex` | the socket list and `playerInfoMap` |
| `g_worldMtx` | the world cell map |
| `g_posMtx` | saved player positions |
| `g_signMtx` | the sign list and the pre-formatted `SIGNP` burst |
| `g_saveMtx` | serialises on-disk writes so two saves cannot interleave (world, players, `eden_bans.txt`, `eden_ops.txt`, `eden_signs.txt`) |
| `g_banMtx` / `g_opsMtx` | the in-memory ban list and op-level table |
| `g_auditMtx` | serialises audit lines so two threads cannot interleave one |

Both command tiers write the world through one shared scan-and-commit path, and it holds
`g_worldMtx` for exactly the model work:

1. the box is volume-checked and the player's cell budget charged **before** the lock is taken;
2. under the lock: look each cell up, decide it, write it, and record it in a batch;
3. after the lock: format the `ACTION:server:…` relay from the batch (`emitEditBatch()`), then
   send it.

Step 3 being outside the lock is not incidental. Measured on the worst case the caps permit — a
131,072-cell edit against a world at its 4,000,000-cell ceiling, every cell changed — step 2 is
~13 ms and step 3 is ~24 ms, because formatting 8.5 MB of `ACTION` text is nearly twice the work
of the map operations it describes. Building the relay under the lock, as the server did before
stage 3.4, made a full-cap edit a ~51 ms stall for every other player. It touches no shared
state, so it moved out; the caps did not have to. **Re-run that measurement before raising any
cap** — the numbers, not the shape of the code, are what says the caps are safe.

Both tiers emit through the same `emitEditWire()`, so there is one `ACTION:server:…` shape to get
right rather than two that can drift.

**The world lock is held for the scan only.** Answering a `REGION` copies matching records into
a local vector under `g_worldMtx`, then releases it — sorting, deflate, base64 and `send()` all
happen unlocked. Holding the world lock across a multi-hundred-millisecond region reply would
queue every other player's edits behind one player's walk.

Per-connection state that only one thread touches carries no lock at all: the `REGION` and
`SIGNQ` burst limiters, the `ACTION` token bucket, and the whole player-command session (its
selection, clipboard, undo history and cell budget) are locals in the client handler. The
connect limiter lives in the single-threaded accept loop for the same reason.

Keeping the player-command session on the connection's own stack is a **design rule, not an
optimisation**. State in a socket-keyed map invites `map::operator[]`, which default-constructs
an entry for a command that arrives before `JOIN`; state keyed by username collides when two
connections share a name. Neither failure is reachable when the state lives on the frame that
owns the connection and dies with it. The single exception is the `/r` reply target, which one
player's thread has to write into another player's slot — so that one piece is global, keyed by
name, and locked (`g_whisperMtx`).

The **failed-auth limiter** (`ewb::AuthFailureLimiter`, `hardening.h`) is the exception among
the rate limiters: it is read in the accept loop (`blocked()` — drop a locked-out IP before a
thread spawns) and written from each client thread (`record_failure()` on a wrong `JOIN`
password), so it is a global behind `g_authFailMtx`. It counts wrong passwords per source IP
in a 60 s window and, past the `--auth-fail-limit` threshold, locks the IP out for a cooldown
that doubles on every further failure (60 s → … → 1 h). The `JOIN` password compare itself is
constant-time (`ewb::const_time_eq`).

## Message framing and dispatch

Client bytes are accumulated into a per-connection buffer and processed one complete
`\n`-terminated line at a time, so TCP coalescing (a `POSVEL` and an `ACTION` in one `recv`) is
handled correctly. A client that floods without ever sending a newline is dropped once the
buffer passes the line cap.

Each line is split on `:` and dispatched by its first field. Unrecognised verbs are logged once
per verb under `--verbose`, so a real client's un-modelled messages surface instead of being
swallowed. See [protocol.md](protocol.md) for the message set.

`REGION` and `SIGNQ` are both gated on a successful `JOIN`: each is a tiny request answered
with a large reply, so requiring the handshake first is both the cheapest anti-amplification
measure and what keeps a passworded world from leaking to a peer that never supplied the
password.

## World model

The base terrain is deterministic on every client, so the server stores only **edits**: a hash
map from a packed `(x, y, z)` key to a `Cell { type, color }`.

| `type` | Meaning |
|---|---|
| `0` | air — the cell was removed |
| `1..254` | a placed block of that type |
| `255` | a natural (base) block that was only painted |

The `255` painted-base value is **internal and never reaches the wire**; the encoder emits a
standalone paint record for such a cell. See [protocol.md](protocol.md) for the mapping.

The key packs x and z into 24 bits each and y into 16, which is where the coordinate bounds
enforced on `ACTION`, `REGION` and sign lines come from. A cap on the number of distinct edited
cells bounds memory and the on-disk file: past the cap, updates to existing cells still apply
and brand-new cells are refused.

Edits are applied by `simAction()`, which mirrors the game's own terrain rules rather than
just recording a delta:

- **build** writes the given type; **mine** writes air; **paint** recolours, promoting an
  untouched natural cell to the painted-base sentinel.
- **burn** on TNT or a firework runs `simExplode()`, a spherical blast that mirrors the game's
  `Terrain::explode`: a coloured centre paints the sphere, an uncoloured one destroys it,
  bedrock and steel survive, and TNT caught in the blast chains (bounded by a recursion depth
  guard). One `ACTION` can therefore write hundreds of cells, which is why burn is charged a
  much higher rate-limit cost than an ordinary edit.

Modelling the rules rather than the deltas is what lets a late joiner be handed a correct
snapshot regardless of the order edits arrived in.

## Persistence

Three plaintext files, all read at startup and all written relative to the process working
directory (see [configuration.md](configuration.md) for the grammars):

| File | Written | Notes |
|---|---|---|
| `eden_world.model` | autosave, and when a client disconnects | only when the dirty flag is set |
| `eden_players.txt` | same | last known position per username |
| `eden_signs.txt` | never | read-only; operator-authored |

Writes are **atomic and serialised**: the snapshot is taken under the data lock, written to a
`.tmp` file, flushed, and `rename()`d over the real file while holding `g_saveMtx`. A crash or
kill mid-write can therefore never leave a truncated world, and a disconnect-save racing the
autosave cannot interleave. If any step fails, the dirty flag is set again so the next save
retries.

Autosave runs every 15 s, so the worst case for an unclean stop is losing one interval.

Signs are parsed once at startup and the entire `SIGNP` burst is formatted into a single buffer
then; every `SIGNQ` re-sends that buffer verbatim. Nothing is rebuilt per request and no lock
is held while formatting. `g_signMtx` exists for a future reload command.

## Startup and shutdown

`main()` parses arguments, clamps out-of-range values, ignores `SIGPIPE`, loads the world,
player positions and signs, starts the autosave thread (and the matchmaker thread if
configured), then binds, listens and accepts. `SO_REUSEADDR` is set on the listener and
`SO_KEEPALIVE` on each accepted socket so peers that vanish without a FIN eventually free their
thread. Because that keepalive reap takes ~2 h on a default Linux, the client handler also sets
`SO_RCVTIMEO` — first to `--handshake-timeout` (a connection that never sends `JOIN` is dropped,
so it cannot squat a client slot), then, after a successful `JOIN`, to `--idle-timeout-conn`
(a joined socket that goes fully silent past that ceiling is dropped; the retail client pings
every 10 s).

Standard output is unbuffered (`std::unitbuf`) so logs appear live under a service manager
rather than being held in a pipe buffer.

Interleaved with those logs is the **audit channel**: one `[Audit] <UTC timestamp> <actor>
<action>` line for every change to server state, from either command tier, emitted through a
single `auditLog()` behind `g_auditMtx` and never gated on `--verbose`. Two tiers keeping their
own log conventions is how they came to disagree about what was worth recording; one function is
what stops that recurring. `--audit-file` appends a second copy, reopened per line so an
operator can rotate or truncate it underneath a running server.

With `--idle-timeout`, the autosave thread exits the process cleanly once the server has had no
clients for that long. That is a normal `exit(0)`, so it must not be combined with a service
supervisor configured to restart unconditionally — it would become a start/idle/exit loop.

## Where to look next

- Wire messages, join order, coordinates: [protocol.md](protocol.md)
- Flags, environment, file formats: [configuration.md](configuration.md)
- Status and planned work: the roadmap in `WORKING/` (untracked, local only).
