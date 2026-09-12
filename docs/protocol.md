# Wire protocol

**Audience:** anyone implementing, debugging or extending a client or server for Eden.

**When you add, remove or change a message the server accepts or emits — including anything in
`region_query.h`, `snapz_codec.h` or `sign_store.h` that shapes the wire — update this file in
the same commit.**

> **Provenance.** This protocol was recovered by observing an unmodified retail client and
> server, and the packet-capture notes in `CAPTURE-FINDINGS.md` (private, not part of this
> repository) are the source of truth for it — they outrank any C++ comment and this document.
> What follows is the set of conclusions the server is built on. Anything **not** established
> by capture is marked ⚠️ **unconfirmed** below; treat those shapes as this server's best
> reconstruction, not as the protocol.

## Transport and framing

- Plain **TCP**, default port **27015**. No TLS, no handshake before the protocol itself.
- Messages are text lines: fields separated by `:`, each line terminated by `\n`.
- The first field is the verb. A verb the server does not know is ignored (and logged once per
  verb under `--verbose`).
- Lines arrive coalesced or split arbitrarily by TCP; both sides must reassemble on `\n`. The
  server drops a connection that accumulates more than 8192 bytes without a newline.
- Because `:` is the delimiter, no username may contain one — see [Usernames](#usernames).
- **Every line the server sends is written whole, by one thread per client.** Nothing can
  interleave inside a line or inside a `SNAPZ` payload, however slow the connection is or
  however many other players are moving. A client that stops reading is refused, disconnected,
  or has stale movement lines dropped — never served half a message. See
  [architecture.md § The output path](architecture.md#the-output-path).
- **Delivery order.** Everything that changes the world — `ACTION` relays, `SIGNP`, `SNAPZ`
  frames — arrives in one stream, in the order the server decided it, so an edit never overtakes
  the bulk reply it belongs after. Movement, chat and `PONG` carry no world state and may
  overtake a large burst; that is what keeps other players visibly moving while one of them is
  downloading terrain.

## Coordinate model

- **x** and **z** are horizontal and centred on **65536** — a fresh world's origin is
  `(65536, ?, 65536)`, not `(0, ?, 0)`. Valid range is `0 .. 0xFFFFFF` (24 bits), which is what
  the server's cell key packs.
- **y** is height, valid range `0 .. 255`.
- Ground standing height is ≈ **33.92** (positions are floats; block coordinates are integers).
- Block **type 0 is air**. In the server's stored model `255` means "a natural block that was
  only painted"; that value is internal and never appears on the wire.

## Client → server

| Message | Notes |
|---|---|
| `JOIN:username:characterType[:password[:clientTag]]` | Once per connection. The retail client sends five fields; the fifth is a constant tag the server neither requires nor interprets. |
| `MSG:text` | Chat. Everything after `MSG:` is the text, so a `:` inside a message is safe. A text starting with `/` is a **command**, answered on the same connection and never relayed — see [commands.md § Part 2](commands.md#part-2--player-commands). |
| `ACTION:x:y:z:mode[:typeOrColor]` | Terrain edit. `mode` 0=build 1=mine 2=burn 3=paint. |
| `POS:x:y:z` | Player position. |
| `VEL:x:y:z` | Player velocity. |
| `POSVEL:px:py:pz:vx:vy:vz` | Both at once. |
| `REGION:x:z` | "Send me the world around this point." Answered with `SNAPZ`. |
| `SIGNQ` | Bare line, no arguments. "Send me the signs." Answered with `SIGNP`. |
| `SIGNP:x:y:z:a:b:c:text` | A player placed or edited a sign. The server's `SIGNP` fields **without** the sender field — see [`SIGNP` from a client](#signp-from-a-client). |
| `PING` | Bare line. Answered with `PONG`. |

`REGION`, `SIGNQ` and `SIGNP` are refused before a successful `JOIN`.

## Server → client

Broadcast to every peer **except** the sender:

| Message |
|---|
| `POS:username:characterType:x:y:z` |
| `VEL:username:characterType:x:y:z` |
| `POSVEL:username:characterType:px:py:pz:vx:vy:vz` |
| `ACTION:username:characterType:x:y:z:mode[:typeOrColor]` ⚠️ unconfirmed |
| `SIGNP:server:x:y:z:a:b:c:text` — a player's sign write, relayed ⚠️ unconfirmed mid-session |
| `[username (T<characterType>)] <chat text>` |
| `[Server] <username> (Type <n>) has joined.` |
| `[Server] <username> has left.` |

Unicast to one client:

| Message | Meaning |
|---|---|
| `[Server] Welcome, <username>! (Character Type: <n>)` | Join accepted. |
| `CAPS:region` | Capability advertisement — see below. |
| `SPAWN:x:y:z` | Restore a saved position. Sent only if this username has one. |
| `SIGNP:server:x:y:z:a:b:c:text` | One sign; sent as a burst answering `SIGNQ`. |
| `SNAPZ:count:base64` | One frame of terrain; sent as a burst answering `REGION`. |
| `PONG` | Answer to `PING`. |
| `ACTION:server:0:x:y:z:1` ⚠️ unconfirmed | Removes a block this player placed that the server refused at the world cell cap. See [At the world cell cap](#at-the-world-cell-cap). |
| `[Server] This world is full, so that edit was not saved. Please tell the server operator.` | An `ACTION` refused at the world cell cap. At most once per 30 s per player. |
| `[Server] This world has reached its sign limit, so that sign was not saved.` | A `SIGNP` refused at the sign cap. |
| `[Server] Invalid name (<reason>).` | `JOIN` refused; connection closed. |
| `[Server] Wrong password.` | `JOIN` refused; connection closed. |
| `[Server] Name already in use.` ⚠️ unconfirmed wording | `JOIN` refused; connection closed. |
| `[Server] Server full.` | Refused at accept; connection closed. |

⚠️ The **peer-relay `ACTION`** shape (`ACTION:<user>:<type>:x:y:z:mode[:extra]`) has not been
confirmed against two retail clients; it was validated against a second independent client
implementation's live parser. The `server`-sender form (`ACTION:server:0:…`) used by the legacy
snapshot is separately community-corroborated but likewise never captured.

## Join sequence

The client sends `JOIN`, `SIGNQ` and `REGION` back to back without waiting for replies, so what
matters is the **order of the server's output**, and that `SIGNP` and `SNAPZ` are *answers*
rather than unsolicited pushes:

```
1.  [Server] Welcome, <name>! (Character Type: N)
2.  CAPS:region
3.  SPAWN:x:y:z          only if this name has a saved position
4.  SIGNP:server:...      burst — answers the client's SIGNQ
5.  SNAPZ:<n>:<b64>       burst — answers the client's REGION
```

`CAPS:region` is what tells a client to *ask* for terrain with `REGION` instead of expecting
the server to push it. A client that has not seen it will not send a `REGION`.

There is deliberately **no chat kill-switch**: the reference server disconnected anyone who
typed "exit" or "quit" in chat, meaning a player discussing quitting got kicked for it. Closing
the game is how you disconnect, and nothing here replaces it.

## `ACTION` — terrain edits

```
ACTION:x:y:z:mode[:typeOrColor]
```

| `mode` | Name | Trailing field |
|---|---|---|
| `0` | build | block type, `0 .. 127` |
| `1` | mine | none |
| `2` | burn | none |
| `3` | paint | paint index, `0 .. 54` (`0` = no paint) |

The server validates coordinates and payload at ingest and **silently drops** anything out of
range rather than closing the connection — a laggy burst from a real player should cost a block,
not a session. Block ids above 127 have never been seen from any client, and 255 in particular
would collide with the model's painted-base sentinel. Paint indices come from a 55-entry
palette; every painted value observed in capture fell in `0..54`, and none was ever a block id.

Modes 1 and 2 carry no payload, so anything trailing them is ignored.

The server does not merely record the edit: it simulates it, including TNT and firework
explosions with chaining. One burn can therefore change hundreds of cells, which is why it is
charged much more heavily against the per-connection edit budget (see
[configuration.md](configuration.md)).

### At the world cell cap

The server holds at most `--max-world-cells` distinct edited cells. At the cap, an edit to a
cell it already holds still applies, but one that would create a **new** cell — a block placed
in open air, a natural block mined or painted — is refused:

- A refused build, mine or paint is **not relayed** to peers, who would otherwise draw
  something no `REGION` will ever send back.
- The player who placed a refused block is sent `ACTION:server:0:x:y:z:1` to take it back out
  of their world (the cell was untouched terrain as far as the server knows, and a client only
  builds into air). ⚠️ This is the relay shape the player commands use; the retail client
  applying it is unconfirmed.
- The player is told `[Server] This world is full, so that edit was not saved. Please tell the
  server operator.`, at most once every 30 s.
- A burn is relayed regardless, since every client simulates the blast itself; if part of the
  blast was refused, the player is told that instead.

Sizing the cap so this never happens is covered in [configuration.md](configuration.md).

## `REGION` → `SNAPZ`

```
client   REGION:<x>:<z>\n        an integer point in absolute world coordinates
server   SNAPZ:<count>:<b64>\n   a burst of frames
```

The reply covers a **chunk-aligned box** around the point, with radius `R` (default 224) and
chunk edge 16:

```
x0 = floor((x - R)/16)*16        x1 = floor((x + R)/16)*16 + 15
z0 = floor((z - R)/16)*16        z1 = floor((z + R)/16)*16 + 15
```

⚠️ `R = 224` is **derived, not measured**: it is the value that fits the observed span of a
captured reply against that formula, with three of four edges landing exactly on the prediction
and the fourth content-bounded. `--region-radius` exists so it can be varied experimentally on
your own server; leave it alone for normal hosting.

Only *edited* cells are sent. The base terrain is deterministic on every client, so an untouched
region legitimately produces zero records.

### Frame format

- `count` is the number of records in this frame. Frames hold up to **3000** records; the last
  one is short. The split is **not** aligned to world chunks.
- The payload is **standard-alphabet base64 with no `=` padding** (`A-Za-z0-9+/`).
- Decoded, it is **raw DEFLATE** — zlib with `windowBits = -15`, no zlib or gzip header and no
  trailing checksum. This server deflates at level 6.
- Inflated length is exactly `count * 20`.
- Each 20-byte record is **five little-endian signed int32**: `x, y, z, flag, type`.

| `flag` | Meaning | `type` field |
|---|---|---|
| `0` | solid block | block type |
| `1` | air | `-1` |
| `3` | paint | paint index, `0..54` |

Cells map to records like this:

| Stored cell | Record(s) emitted |
|---|---|
| type `0` (air) | `flag 1`, type `-1` |
| type `255` (painted natural block) | `flag 3`, paint index |
| type `1..254`, no paint | `flag 0`, type |
| type `1..254`, painted | `flag 0`, type **and** `flag 3`, paint index |

Note that air on the wire is `-1`, never `0` and never `255`. A paint value outside `0..54` is
**dropped, not clamped**: a solid block then simply renders unpainted, and a painted-base cell
with no valid colour describes no edit at all and is omitted entirely.

Record order is not semantically significant — a client must merge the `flag 0` and `flag 3`
records for a cell in either order. This server sorts by `(z, x, y, flag)` before deflating
purely because sorted records compress materially better than hash-map order;
`--no-region-sort` disables that.

An **empty region is answered with `SNAPZ:0:<b64>`** — a well-formed frame decoding to zero
records. A real frame tells a client "answered, nothing here", where silence leaves it waiting
for a snapshot that never comes. ⚠️ The retail server has never been *observed* answering an
empty region; `--no-region-empty-frame` restores silence for comparison testing.

### Pacing

`REGION` is the single most expensive thing a client can ask for and a textbook amplification
vector — roughly 20 bytes in, potentially megabytes out. The server therefore enforces a
minimum gap between served requests and a per-session cap. Requests inside the gap are
**dropped, not queued**; queueing would hand the amplification straight back. `SIGNQ` gets the
same treatment for the same reason. Current values are in
[configuration.md](configuration.md).

A request can also be refused for **backpressure**: a client that already has
`--client-region-queue` replies in flight, or a server whose total scanned-but-unsent record
backlog is at `--region-pending-records`, answers nothing rather than starting a burst it cannot
finish. A refused request is silence, exactly like one inside the gap — a partially delivered
region is the failure this is here to avoid, so refusing is the honest answer and the client
re-asks. (Silence is the status quo rather than a decision to keep forever: a new "throttled"
line the retail client has never been observed receiving is a protocol risk, so it has not been
invented. A client cannot currently distinguish "throttled" from "empty region", and
`--no-region-empty-frame` makes that worse rather than better.)

## `SIGNQ` → `SIGNP`

```
client   SIGNQ\n
server   SIGNP:server:<x>:<y>:<z>:<a>:<b>:<c>:<text>\n     (a burst, no terminator)
```

Field 2 is the literal token `server` — the same reserved sender token `ACTION:server:0:…`
uses, which is why `server` is refused as a username.

There is **no terminator**: the burst simply ends. (A different subprotocol does terminate its
listing; do not generalise one to the other.) A world with no signs is answered with silence,
which is what "nothing" means here.

⚠️ `a`, `b`, `c` are **unknown fields**. The server emits whatever the sign file holds,
verbatim, and invents no semantics for them.

### `SIGNP` from a client

```
client   SIGNP:<x>:<y>:<z>:<a>:<b>:<c>:<text>\n
```

The retail client sends this when a player places or edits a sign. It is the server's line
without the sender field; a line carrying `server` there fails to parse and is ignored.

- `x, y, z` is the **block the sign is attached to**, not the air cell in front of it: a sign
  placed and later removed in game is placed and mined at the same coordinates.
- The sign goes into its **slot — the block `x, y, z` plus `a`** — replacing any sign already
  there. Real worlds hold two signs on one block that differ in `a`, and `a` has only ever been
  seen as `0..5`, which fits a block face. That is inferred from data, not confirmed.
  Re-sending an identical sign changes nothing.
- The text gets the same treatment as the sign file: capped at 256 bytes, control characters
  stripped.
- The next `SIGNQ` answer includes it immediately. `eden_signs.txt` is written on the next
  autosave, when the player disconnects, or on the control socket's `save`/`stop`.
- It is relayed to every other player as `SIGNP:server:…`. ⚠️ Whether a connected retail client
  applies a `SIGNP` outside its join burst is unconfirmed; one that doesn't sees the sign on
  its next join.
- It is refused before `JOIN`, paced per connection, and refused with a chat line to the
  player once the world holds 20,000 signs. Values are in [configuration.md](configuration.md).
- **The client sends nothing when a sign is removed.** In game a sign goes only when the block
  it is attached to does, and the server sees just that block's `ACTION`. So the server removes
  **every** sign on a block, whatever its `a`, whenever any edit stores air there: a mine, a burn
  or explosion, the control socket's `setblock`/`fill`, a player command, `//undo` or `//redo`.
  An edit refused at the world cell cap leaves the block, and its signs.
- No message is sent for that removal. Peers already get the block's own relay, and a client
  does not show a sign whose block is air (⚠️ inferred from a player's report, not captured).
  The next `SIGNQ` answer leaves the sign out, and `eden_signs.txt` loses it on the next save.
- `//undo` of an edit that removed a sign restores the block, not the sign.

## `PING` → `PONG`

A bare `PING` line is answered with a bare `PONG` line — no arguments echoed. That is what
gives a client a real round-trip time.

This is unrelated to the `PING:<playercount>` heartbeat the server sends *outbound* to a
matchmaker when `--matchmaker` is configured.

## Usernames

Enforced at `JOIN`; a name that fails is refused with a reason and the connection is closed.

- 1 to 20 bytes.
- Printable ASCII only (`0x20`–`0x7E`). Bytes ≥ `0x80` are rejected — a bounded charset is the
  point of the rule.
- No `:` (the field delimiter), no `[` or `]` (they would forge the `[name (Tn)]` chat
  structure), no leading or trailing space.
- `server` is reserved, case-insensitively — it is the sender token clients trust.
- A name already connected is refused. Saved positions are keyed by username, so two players
  sharing one would share and clobber a single slot.

`characterType` is accepted in `0..255` and echoed back in the welcome line. ⚠️ Its exact
meaning is unsettled: a retail client was observed sending 17 where the retail server's welcome
line said 0, which is consistent either with clamping or with the field not being a plain
character index at all. Accepting the wider range is strictly safer than silently rewriting a
legitimate value.

## Chat

Client `MSG:text` is rebroadcast to every other peer as:

```
[<username> (T<characterType>)] <text>
```

Text is capped in length and stripped of ASCII control characters before relay (bytes ≥ `0x80`
survive, so UTF-8 chat is intact). The username charset rule is the other half of that guard.

**A `MSG:` whose text begins with `/` is not chat.** It is a player command; the server runs it
and answers this connection only, with `[Server] …` lines (plus `SPAWN:` for a teleport,
`SNAPZ:` for `/resync`, and `ACTION:server:…` to every client for a build command). It is never
rebroadcast as chat — doing so would leak one player's `/msg` text to the whole server. Commands
before a successful `JOIN` are ignored in silence. The full vocabulary, its permission levels
and its bounds are in [commands.md § Part 2](commands.md#part-2--player-commands); nothing about
it changes the wire, which is why it is documented there rather than here.

## Direct connection

Retail clients have **no "connect to IP" field**; the shipped flow is the in-game Server
Browser, which lists servers from a first-party matchmaker. Two things follow:

- For testing and for LAN play, the retail client honours an environment variable
  (`EDEN_MP_AUTOJOIN`, value `host:port`) that makes it connect directly on launch. See
  [configuration.md](configuration.md#client-side).
- For a server to appear in the in-game browser, it must be listed by a matchmaker. The
  matchmaker wire protocol (`REGISTER` / `PING` / `LIST` / `HOST`) is documented in
  [matchmaker.md](matchmaker.md), and this repo ships both sides of it — the `--matchmaker`
  client in `edenserver` and the standalone `edenmatch` server. Pointing the *retail* client at
  a non-default matchmaker still requires a build change it does not expose, so `edenmatch` is
  for LAN / testing / private lists today — see
  [quickstart.md § What this does not cover](quickstart.md#what-this-does-not-cover).

## Legacy world snapshot

Before `REGION`/`SNAPZ` was implemented, a joining client was pushed the whole world as a
stream of `ACTION:server:0:…` lines. That path still exists behind `--legacy-snapshot` and is
**off by default**: no retail client has been observed receiving it, and its wire shape is only
community-corroborated. It is kept as a bring-up fallback and as the only path that works
against a client that never sends `REGION`.

⚠️ Within it, a server-pushed edit to an occupied cell must mine before it builds — a bare
build does not overwrite an occupied cell, so the mine→build→paint ordering is load-bearing.
The operator control socket's `setblock` / `fill` relay their edits the same way
(`ACTION:server:0:…`, mine→build→paint), so connected clients apply an admin edit without
re-requesting the region.

## Operator commands are out of band

There is no admin command *on the wire*. Operator control (`kick`, `ban`, `save`, `stop`,
`setblock`, `fill`, live sign editing, `region-stats`) is a local `0600` unix domain socket,
never the game connection — see [commands.md](commands.md). `JOIN` carries a shared world
password, not a per-user credential, so there is no identity on the wire to build admin
authorisation on; filesystem permissions on the socket are the authorisation instead.
