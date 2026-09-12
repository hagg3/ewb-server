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
out_queue.h          Per-client output queue policy: the two queues (latency-sensitive
                     lines vs. the ordered world-state stream), region replies as
                     lazily-encoded jobs, drain order, byte accounting, and the two
                     overflow policies.
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
eden_file.h          Clean-room parser for the .eden world file format: ZIP-wrapper
                     detect + bounded inflate, the 192-byte header, chunk-size
                     detection, the directory coordinate gate, per-chunk derived
                     spans, bounded voxel reads, the sidecar + inline sign parsers.
                     Pure; consumed by eden_import.
eden_import.h        The .eden -> server-world conversion core: the base terrain profile,
                     the diff/solid/full emitter, the axis rename for blocks and signs,
                     the cell and worst-case-REGION projections, the output line
                     grammars. Pure; no file I/O.
eden_import.cpp      The eden_import CLI: argument parsing, file I/O, atomic writers,
                     the summary. A third single translation unit. Offline tool, run
                     before the server starts — see docs/import.md.
eden_fixture.h       Test-only: synthesized .eden files for the two suites below, so
                     there is one writer of the format the parser reads. Not compiled
                     into any shipped binary.
```

Each header has an offline test binary built and run by `build_server.sh`:

```
snapz_codec_test.cpp   SNAPZ round-trip (deflate/base64/frame).
region_test.cpp        Region box, cell encoding, frame splitting.
out_queue_test.cpp     The no-split invariant (a queue entry is always a whole line or a
                       whole frame), drain order, world-state FIFO, byte accounting, both
                       overflow policies, region-job admission, a whole region draining to
                       exactly its input records.
protocol_test.cpp      Signs, usernames, ACTION validation, rate limiters.
control_test.cpp       Control line grammar, command table, ban/ops files, fill bounds,
                       the derived fill cap, the flood guard's state machine.
worldedit_test.cpp     Player command table + permission floors, grammar, selection cap,
                       shape predicates, undo byte budget, clipboard rotation.
matchmaker_test.cpp    Name sanitising, REGISTER parsing, SERVER: row / LIST, the registry.
eden_file_test.cpp     .eden header decode, chunk-size detection (version / creature-gap /
                       min-gap), directory gate, derived spans + bounded voxel reads,
                       sidecar + inline sign parsing, ZIP member select + bomb cap.
eden_import_test.cpp   The base terrain profile, the diff/solid/full emitter, the axis
                       rename, the two budget projections, the anomaly counters, and the
                       golden end-to-end: a synthesized .eden converted to an
                       eden_world.model diffed byte-for-byte against
                       testdata/carved_64z.model. Its last group shells out to
                       ./eden_import for the CLI's refusals and --dry-run, so run the
                       suite from the repo root.
```

Supporting files: `build_server.sh` (build + run all suites), `host_world.sh`
(convenience launcher), `edenctl` (client for the operator control socket), `run_server.bat`
(Windows/MSVC launcher), the `phase3_live_test.py` and `phase7_live_test.py` scripts (run by
hand, not by the build — they bind a port and spawn processes), `worlds/<name>/` (sample
worlds), `testdata/` (committed golden files for the offline suites).

`admin/` is a separate, optional Go module — `edenadmin`, a local operator GUI that drives
`edenctl` / `eden_import` / `ssh` (`admin/README.md`). It is not built by the default
`build_server.sh` and is not needed to run a server; `./build_server.sh --with-admin` or
`admin/build.sh` builds it. The server stays a single POSIX translation unit plus pure headers.

`phase3_live_test.py` is the adversarial counterpart to the offline suites: the suites prove the
command-surface bounds are correct, and it tries to break them over real sockets — oversized
selections against every command that reads one, permission escalation by casing and prefix
tricks, undo growth across a long session, command flooding, malformed-argument fuzzing, and the
control socket's own pacing and connection cap. Run it before hosting anything publicly.

`phase7_live_test.py` is the counterpart for the output path, and it exists because that failure
mode is **invisible on a fast link** — a test that reads promptly proves nothing. Every group in
it drives a client reading at roughly 50 KB/s through a tiny receive window: a multi-frame
`SNAPZ` burst while another player moves at 50 Hz (no short, undecodable or displaced frames,
every record delivered), a third client's `JOIN` completing in under 2 s while another client is
backed up, a client that never drains being disconnected rather than parking a thread, a name
freed the instant its socket goes, a `REGION` past the queue depth being refused rather than
half-served, and a client too far behind on world state being dropped — with everyone else
carrying on — rather than quietly missing edits. Run it after touching anything in the output
path.

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
| client writer | one per accepted socket, **joined** by its own handler | the only thread that ever writes that socket: drains the client's output queue, encodes `SNAPZ` frames, enforces the write timeout |
| autosave | at startup, detached | every 15 s: `saveWorld()`, `savePlayerPos()`, `saveSigns()`, and the `--idle-timeout` check |
| matchmaker | at startup **iff** `--matchmaker` was given, detached | keeps one TCP registration open, heartbeats a player count every ~15 s, reconnects on failure |
| control listener | at startup unless `--no-control-socket`, detached | `accept()` loop on the `0600` unix domain socket |
| control handler | one per control connection, **detached** | reads `\n`-framed command lines, runs each, replies; `stop` saves and exits the process |

Every thread is detached **except a client's writer**, which is joined by that client's own
handler and by nobody else. A client handler removes itself from the roster, retires its writer,
and only then closes the socket, so no handle accumulates and no thread can write a descriptor
the kernel has already recycled. `SIGPIPE` is ignored at startup so a peer that vanishes
mid-write cannot take the process down. An exception escaping a thread would `std::terminate`
the whole server, so the paths that can throw (the SNAPZ encoder) catch locally.

### The output path

**One writer thread per client owns every write to that socket. Nothing else calls `send()` on a
player's socket.** Producers — broadcasts, region replies, chat, the join sequence, kick notices
— enqueue and return; they never touch the network.

This is structural rather than a lock, and it has to be. Before it, a `SNAPZ` burst was streamed
with no lock held while other threads wrote the same descriptor, and a blocking `send()` bigger
than the send buffer yields while it waits — so another player's movement update landed *inside*
a base64 payload. Records are sorted and framed at a flat 3000, so one destroyed frame is one
1-block-wide row across the whole reply box: players saw long strips of the world reset, only on
weak connections. A per-socket write mutex would have fixed that and made the next problem
worse, because a blocked burst would hold it while a broadcaster waited on it under
`clientsMutex`. With a queue, an interleaved write is not something callers must remember to
avoid — it cannot be expressed.

Each client has two queues ([out_queue.h](../out_queue.h) has the full rationale):

- **latency-sensitive**: `POS`/`VEL`/`POSVEL`, `PONG`, chat, `[Server]` notices, the welcome,
  `CAPS`, `SPAWN`. Drained first. Order-insensitive, so when the backlog passes
  `--client-outbox-max` the *oldest* lines are dropped — a four-second-old position is worthless,
  and disconnecting instead would punish exactly the weak-link players this exists for.
- **world state**: `ACTION` relays (single and batch), `SIGNP` writes and bursts, `SNAPZ` region
  frames, the legacy snapshot. One FIFO, so a block edit can never overtake the bulk reply it
  belongs after. Nothing here is ever dropped: a client-initiated request (`REGION`, `SIGNQ`)
  past its budget is refused and re-asked, and a broadcast relay past `--client-world-max`
  disconnects that client, which resyncs properly on rejoin.

A `REGION` reply is queued as a **job** — the scanned, sorted record vector plus a cursor — and
the writer encodes one frame per turn. The client's own thread therefore returns as soon as the
scan is done, deflate runs off it, and the memory an in-flight region costs stays the record
vector instead of gaining up to ~17 MB of encoded base64 on top. `--region-pending-records`
bounds those vectors across all clients; the accounting rides on the vector's own deleter, so it
stays correct however a job ends.

The split also separates two numbers that used to be one: the `REGION` log line reports scan and
sort from the client's thread, and the writer reports encode and drain when the burst completes.
Before, `encode N ms` silently spanned encode *and* send, so an operator could not see
backpressure as backpressure.

### Locks

| Mutex | Guards |
|---|---|
| `clientsMutex` | the socket list and `playerInfoMap` |
| `g_worldMtx` | the world cell map |
| `g_posMtx` | saved player positions |
| `g_signMtx` | the sign list, the pre-formatted `SIGNP` burst, the index of signed blocks and the queue of sign-removal audit lines. Taken after `g_worldMtx` when an edit turns a signed block to air, so nothing holding it may take `g_worldMtx` |
| `g_signSaveMtx` | spans `saveSigns()`' snapshot and write, so of two racing sign saves the newer list is the one left on disk. Taken before `g_signMtx`, never after it |
| `g_saveMtx` | serialises on-disk writes so two saves cannot interleave (world, players, `eden_bans.txt`, `eden_ops.txt`, `eden_signs.txt`) |
| `g_banMtx` / `g_opsMtx` | the in-memory ban list and op-level table |
| `g_auditMtx` | serialises audit lines so two threads cannot interleave one |
| `g_outsMtx` | the socket → client-writer map. Held for a map lookup or walk and nothing else |
| `ClientOut::m` | one client's output queue and writer state |

**Lock order: any lock above → `g_outsMtx` → `ClientOut::m`.** The last two are terminal —
nothing inside either critical section takes another lock, and no syscall runs under either — so
the ~30 places that produce output do not have to reason about ordering at all. A broadcast
copies owning pointers out under `g_outsMtx`, releases it, and only then enqueues, which is both
why a client departing mid-broadcast stays alive for the enqueue and why one backed-up client can
no longer freeze every join, chat line and edit on the server.

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
a local vector under `g_worldMtx`, then releases it — sorting happens unlocked, and encoding and
sending happen later, on the client's writer thread. Holding the world lock across a
multi-hundred-millisecond region reply would queue every other player's edits behind one
player's walk.

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
and brand-new cells are refused. `worldSet()` reports each refusal and `simAction()` counts
them, so the `ACTION` handler can keep a refused edit off the peers and tell the player rather
than drop it silently ([protocol.md](protocol.md#at-the-world-cell-cap)).

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

**Signs go with their block.** A sign's `x, y, z` is the block it is attached to, and the
client sends nothing when a sign is removed. `worldSet()` is the only live writer of the map,
so it is where every edit that stores air passes — a mine, a blast, `setblock`/`fill`, a player
command, `//undo` — and it removes every sign on that block there. A hash set of signed blocks
keeps that to one lookup per cell, and a world with no signs skips it entirely. The removal is
recorded under the world lock; its audit line and the rebuilt `SIGNQ` burst wait until the
lock is released, so a `fill` over many signed blocks formats the burst once, not per sign.

## Persistence

Plaintext files, all read at startup and all written relative to the process working
directory (see [configuration.md](configuration.md) for the grammars):

| File | Written | Notes |
|---|---|---|
| `eden_world.model` | autosave, and when a client disconnects | only when the dirty flag is set |
| `eden_players.txt` | same | last known position per username |
| `eden_signs.txt` | autosave, disconnect and control `save`/`stop` after a player's sign write or an edit that removed signs with their block; at once on control `signs add`/`rm`, and at startup or `signs reload` when signs on blocks stored as air were dropped | only when the sign list changed |
| `eden_spawn.txt` | never | read-only; the default spawn for a player with no `eden_players.txt` row. `--spawn`/`--spawn-file` override. Malformed → one warning, ignored |

Writes are **atomic and serialised**: the snapshot is taken under the data lock, written to a
`.tmp` file, flushed, and `rename()`d over the real file while holding `g_saveMtx`. A crash or
kill mid-write can therefore never leave a truncated world, and a disconnect-save racing the
autosave cannot interleave. If any step fails, the dirty flag is set again so the next save
retries.

Autosave runs every 15 s, so the worst case for an unclean stop is losing one interval.

Signs are parsed at startup and the entire `SIGNP` burst is formatted into a single buffer;
every `SIGNQ` re-sends that buffer verbatim. Nothing is rebuilt per request: the burst is
rebuilt under `g_signMtx` only when the sign list changes (a player's sign, `signs add|rm|reload`,
or an edit that removed signs with their block).

## Startup and shutdown

`main()` parses arguments, clamps out-of-range values, ignores `SIGPIPE`, loads the world,
player positions and signs (then drops signs on blocks the world stores as air), starts the autosave thread (and the matchmaker thread if
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
