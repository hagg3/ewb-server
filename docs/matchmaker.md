# Matchmaker (`edenmatch`)

Eden's retail client has no "connect to IP" field. The way onto a server is the in-game
**Server Browser**, which is populated by a *matchmaker*: game servers hold an open TCP
connection to it and register themselves; game clients connect, ask for the list, and
disconnect.

`ewb-server` ships its own matchmaker, **`edenmatch`**, plus the `--matchmaker` client in the
server itself. Point a server at any matchmaker (yours or another operator's) with:

```
./edenserver --matchmaker <matchmaker-host>[:27020] --advertise <public-ip-of-the-server>
```

> **Reaching the *first-party* list.** The retail game's own matchmaker is at a fixed address.
> Whether the retail client can be pointed at a *different* matchmaker without a build change is
> the same open question as direct-connect (it cannot, today, except via `EDEN_MP_AUTOJOIN`).
> `edenmatch` is therefore useful right now for **LAN play, testing, and a private
> friends-list** — anywhere you also control which matchmaker the clients use — and is the
> reference implementation of the protocol if first-party listing later opens up. See
> [quickstart.md § What this does not cover](quickstart.md#what-this-does-not-cover).

## Protocol

Newline-framed, `:`-delimited, one message per line, lines capped at ~4 KB. Raw TCP on port
**27020** by default. Three kinds of peer.

### Game server → matchmaker (persistent)

| Send | Reply | Notes |
|---|---|---|
| `REGISTER:<name>:<port>:<hasPassword>[:<advertiseIP>]` | `REGISTERED` | The registration lives as long as the TCP connection stays open. `hasPassword` is `0`/`1`. If `advertiseIP` is omitted or empty, the matchmaker uses the connection's peer address. `advertiseIP` must be a dotted IPv4 address, and `edenmatch` only lists it as sent under the rules in [Advertised addresses](#advertised-addresses). `edenmatch` can also reply `REGISTERFAIL:full` (registry full), `REGISTERFAIL:taken` (another peer's live server holds that `ip:port`) or `REGISTERFAIL:limit` (this address already has 8 rows). |
| `PING[:<players>]` | *(none)* | Keep-alive. Resets the ~45 s TTL. `<players>` (optional) reports the live join count, clamped to `[0, 1000]`; a bare `PING` leaves the last reported count as-is rather than clearing it. Junk after the colon is treated the same as a bare `PING` — the heartbeat itself is never rejected. `ewb-server` sends one every ~20 s, with the count reflecting joined players (not raw accepted sockets). |

#### Server names

A registered name is sanitised before it goes anywhere near the browse list, but only as far as
wire safety actually requires: `:` and every control byte (including `\n` and `\r`) are dropped,
TAB and runs of spaces collapse to a single space, and the result is trimmed and capped at **48
bytes** — comfortably inside the client's 64-byte server-name field. Everything else printable
survives, so punctuation, apostrophes and non-ASCII names come through intact (`Ari's Server`
lists as `Ari's Server`). Truncation never splits a UTF-8 code point. A name that sanitises to
nothing is rejected outright, and `REGISTER` fails.

This is deliberately *not* the same function as the `HOST` world-file slug below, which has to be
a safe path component and is therefore much narrower.

#### Advertised addresses

The `ip` in a browser row is the address every client will dial, so `edenmatch` does not take a
server's word for it. A row is keyed on `ip:port`, and before these rules any peer could register
another server's address and take over its row.

- `advertiseIP` must parse as a dotted IPv4 address. Anything else — a hostname, a truncated
  address, a stray control byte — fails the whole `REGISTER`, and the connection is closed.
- It is listed as sent only if it is the connecting peer's **own** address, or equals
  `--advertise-ip`, or the peer is **trusted**. Otherwise the row lists the peer address instead,
  and `edenmatch` logs the substitution. A peer can't forge its TCP source address, so no remote
  peer can list a row under somebody else's address.
- **Trusted** peers are loopback (`127.0.0.0/8`, a process on the matchmaker's own host — including
  every `HOST` spawn) plus anything listed in `--trust-advertise`.

In practice: a server on the matchmaker's own host, or on a network listed in `--trust-advertise`,
can advertise any address (its public IP, its LAN IP). A server that connects from anywhere else
is listed under the address it connects from. For a server behind NAT that is its router's public
address, which is the one remote clients need. `edenserver` advertises its LAN address by default,
so this is the usual case for a home-hosted server.

Re-registering with the same `ip:port` **replaces** the old entry (dedupe — keyed on `ip:port`,
not `name:ip:port`, so a server that renames itself doesn't appear twice) but **carries the
player count forward**, so a rename or reconnect does not flicker the row back to `0`. Who may
replace a row:

- **the connection that owns it** — a rename;
- **another connection from the same peer address** — a restarted server whose old socket hasn't
  errored yet. `edenmatch` closes the old connection rather than leaving it open;
- **anyone**, if the row is orphaned (see below);
- **nobody else.** A live row owned by a different peer address gets `REGISTERFAIL:taken`.

Each connection owns at most one row: a re-`REGISTER` on a new port moves the row, it doesn't add a
second one. An untrusted peer address may hold at most **8** rows (`REGISTERFAIL:limit` past that),
so a handful of source addresses can no longer fill the 512-row registry and lock every real
server out.

**A dropped registration socket does not immediately delist the server.** The entry is orphaned
(kept, with no owning connection) instead, so a brief network hiccup doesn't vanish a server that
is plainly still up. Every ~10 s, `edenmatch` sweeps entries that have gone quiet for 45 s: before
dropping one it TCP-probes the address it advertised (~2 s timeout), and keeps — refreshing —
anything that still answers. Only an entry that is both quiet *and* unreachable is actually
removed.

`edenmatch` also persists the live registry to disk (`eden_registry.txt` by default, atomic
temp+rename, rewritten on the same ~10 s tick) and reloads it at startup. Reloaded entries come
back orphaned and immediately eligible for the probe above, so a matchmaker restart re-verifies
each one instead of trusting a possibly-stale file — the browser survives a restart without going
empty until every server's next reconnect. `--registry-file PATH` overrides the path;
`--registry-file ""` disables persistence entirely.

`edenmatch` also caps concurrent connections — 256 total, 24 per source IP — refusing (and
logging) anything past that instead of spawning an unbounded number of threads.

**Connection lifecycle.** A slot is only useful if it is eventually given back, so every
connection has a read deadline:

- A fresh connection must send its first line (`LIST`, `HOST:…` or `REGISTER:…`) within **15 s**
  or it is closed. Every legitimate peer sends it immediately; a peer that connects and says
  nothing would otherwise hold a thread and a cap slot forever, and a few dozen such peers would
  make every Server Browser `LIST` refuse. Replies carry the same 15 s send deadline, so a peer
  that asks for a list and never reads it cannot pin a slot from the other side.
- A connection that has been accepted as a **registration** is the one peer that may go quiet, so
  its deadline is relaxed to the 45 s heartbeat TTL plus 15 s of slack (60 s). Silence past that
  closes the socket and orphans the row as described above; the sweep then decides its fate.
- A `HOST` request holds its connection (and its slot) while the spawned server registers back,
  up to 30 s. That is bounded and only reachable with `--allow-host`; it is not moved off the
  connection because the requester is waiting on that same socket for the reply.

`accept()` failures follow the same policy as `edenserver`: routine ones (`EINTR`,
`ECONNABORTED`) retry silently, descriptor or memory exhaustion (`EMFILE`, `ENFILE`, `ENOBUFS`,
`ENOMEM`) sleeps 100 ms before retrying so the loop cannot spin a core, and anything else is
logged at most once a second.

### Game client → matchmaker (one-shot; connection closed after the reply)

| Send | Reply |
|---|---|
| `LIST` (also accepted: `LISTP`) | `SERVER:<name>:<ip>:<port>:<locked>:<players>:<flag6>` per server, then `END` |
| `HOST:<name>[:<hasPassword>[:<password>]]` | `HOSTED:<ip>:<port>` on success, else `HOSTFAIL:full` / `HOSTFAIL:spawn` / `HOSTFAIL:timeout` |

#### Why seven fields

Three sources describe the `SERVER:` row and all three disagree on its width:

| Fields | Source | Form |
|---|---|---|
| 4 | a developer prose sketch of the protocol | `SERVER:<name>:<ip>:<port>:<hasPassword>` |
| 6 | a community-supplied matchmaker implementation | `SERVER:<name>:<ip>:<port>:<hasPassword>:<players>` |
| **7** | **a byte-exact capture of the live matchmaker's browse reply** | `SERVER:<name>:<ip>:<port>:<locked>:<players>:<flag6>` |

`edenmatch` emits **7 by default**. The capture is the only one of the three read off the wire of
the server the retail client actually browses, and in it the 6th and 7th columns move
independently of each other — the count column tracks live joins, while the last column stays set
for a fixed subset of servers across separate passes. A 7th column carrying its own meaning is not
a mis-split or a delimiter artifact of a 6-field row, so the 6-field implementation is an older or
divergent branch rather than the running build.

Both narrower forms are kept for an A/B: `--prod-list` emits the 6-field form, `--short-list` the
4-field sketch form. `flag6` appears to be an opaque mode/PvP bool; `edenmatch` has no way to set
it, so it is always `0` for servers registered with it.

The empty list is a bare `END` (the natural reading of the grammar; the empty case was never
captured). Rows are sorted players-descending, then by name.

`HOST` is on-demand world creation. `edenmatch` only honours it with `--allow-host`, which lets
it `fork`+`exec` an `edenserver` on a free port from `--host-ports LO-HI` and wait for that
server to register back before replying `HOSTED`. The name determines what happens:

- a name that is already **live** (has a current registration) returns that running server's
  `HOSTED:ip:port` — no second process is spawned, so two `HOST` calls for the same name can't
  end up fighting over one world file;
- a name with no live registration spawns `edenserver --world world_<slug>.model`, where
  `<slug>` is the name lowercased, folded to `[a-z0-9]` with every other run of bytes collapsed
  to a single `_` — a name that already has a saved world under that slug reloads it, otherwise
  the server starts with an empty world;
- only rows registered **from the matchmaker's own host** count as "live" here, and a spawn is only
  considered registered once a loopback connection registers its port — a remote peer can't
  pre-register a name or a port and have `HOST` hand requesters its address;
- the spawned server's `--advertise` (and the `HOSTED` reply) use `--publicip` if set, else
  `--advertise-ip`, else the HOST requester's own peer address — *not* a hardcoded loopback, so a
  world hosted for a remote client is reachable by other remote clients too.

### Matchmaker → spawn agent

The retail matchmaker can offload spawning to an agent process on the game-server host
(`SPAWN:port:name:hasPassword:password` → `SPAWNED` / `SPAWNFAIL`). `edenmatch` does not
implement the agent role — it spawns locally under `--allow-host` instead.

## `edenmatch` flags

| Flag | Default | Meaning |
|---|---|---|
| `--port N` | `27020` | TCP port to listen on (all interfaces). A bare leading number also works. |
| `--advertise-ip IP` | *(peer address)* | The address listed for a registration that sends no `advertiseIP`, and the one a foreign `advertiseIP` falls back to — use it when every server is on the matchmaker's host behind one public address. Any peer may also advertise it explicitly. Must be IPv4; `edenmatch` refuses to start otherwise. |
| `--trust-advertise LIST` | *(loopback only)* | Comma-separated IPv4 addresses and/or CIDR ranges (`192.168.1.0/24,203.0.113.5`) whose `REGISTER` may advertise an address other than their own, and which aren't held to the 8-rows-per-address cap. Loopback is always trusted. `0.0.0.0/0` restores the old trust-everyone behaviour — only for a matchmaker nobody hostile can reach. See [Advertised addresses](#advertised-addresses). |
| `--short-list` | off | Emit the 4-field developer-sketch `SERVER:` row instead of the 7-field default. |
| `--prod-list` | off | Emit the 6-field `SERVER:` row (no `flag6`) instead of the 7-field default. |
| `--verbose` | off | Log every `LIST`, `HOST`, bad line, and unknown verb, not just registrations. |
| `--allow-host` | off | Honour `HOST:` by spawning a local `edenserver`. |
| `--edenserver PATH` | `./edenserver` | Binary to spawn for `HOST` (with `--allow-host`). |
| `--world DIR` | `.` | Working directory for a spawned server; each `HOST` name gets its own `world_<slug>.model` inside it (world/player/sign files land alongside it). |
| `--host-ports LO-HI` | `27600-27699` | Port range `HOST` allocates from. |
| `--publicip IP` | *(unset)* | Address to advertise for a `HOST`-spawned server (both the `HOSTED` reply and the child's own `--advertise`). Falls back to `--advertise-ip`, then the `HOST` requester's peer address. Must be IPv4. |
| `--registry-file PATH` | `eden_registry.txt` | Where the live registry is persisted (atomic temp+rename, rewritten every ~10 s). `--registry-file ""` disables persistence. Reloaded entries are re-verified by probe before being trusted — see above. |

Registrations are otherwise held in memory; a restart still requires each server's own reconnect
(`ewb-server` does this automatically within a few seconds) for a *fully* fresh row, but the
persisted file keeps the browser populated with probe-verified entries in the meantime.

## Running it

```
./build_server.sh          # builds ./edenmatch alongside ./edenserver
./edenmatch --verbose       # listen on 27020

# in another shell, on the same host (loopback is trusted, so --advertise is honoured):
./edenserver --port 27015 --name "My World" --matchmaker 127.0.0.1:27020 --advertise 192.168.1.20

# servers on other LAN machines advertising addresses other than their own need, e.g.:
./edenmatch --trust-advertise 192.168.1.0/24
```

Firewall: open TCP **27020** only if you are hosting a matchmaker other machines must reach.
See [`ops/INSTALL.md`](../ops/INSTALL.md).
