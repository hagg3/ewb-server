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
| `REGISTER:<name>:<port>:<hasPassword>[:<advertiseIP>]` | `REGISTERED` | The registration lives as long as the TCP connection stays open. `hasPassword` is `0`/`1`. If `advertiseIP` is omitted or empty, the matchmaker uses the connection's peer address (fine on a LAN, wrong behind NAT — pass it explicitly for a public server). `edenmatch` also replies `REGISTERFAIL:full` if its registry is full. |
| `PING` | *(none)* | Bare keep-alive. Resets the ~45 s TTL. `ewb-server` sends one every 20 s. |

Re-registering with the same `name:ip:port` **replaces** the old entry (dedupe). Disconnecting
removes the server from the list. `edenmatch` also drops any registration it has not heard from
within 45 s, as a backstop for a wedged connection.

### Game client → matchmaker (one-shot; connection closed after the reply)

| Send | Reply |
|---|---|
| `LIST` (also accepted: `LISTP`) | `SERVER:<name>:<ip>:<port>:<locked>:<players>:<flag6>` per server, then `END` |
| `HOST:<name>[:<hasPassword>[:<password>]]` | `HOSTED:<ip>:<port>` on success, else `HOSTFAIL:full` / `HOSTFAIL:spawn` / `HOSTFAIL:timeout` |

The `SERVER:` row grammar is the byte-exact form observed from the retail matchmaker. A
developer sketch of the protocol describes a shorter 4-field row
(`SERVER:<name>:<ip>:<port>:<hasPassword>`); `edenmatch --short-list` emits that instead, for
testing against a client that turns out to want it. The empty list is a bare `END` (the natural
reading of the grammar; the empty case was never captured).

`HOST` is on-demand world creation. `edenmatch` only honours it with `--allow-host`, which lets
it `fork`+`exec` an `edenserver` on a free port from `--host-ports LO-HI` and wait for that
server to register back before replying `HOSTED`.

### Matchmaker → spawn agent

The retail matchmaker can offload spawning to an agent process on the game-server host
(`SPAWN:port:name:hasPassword:password` → `SPAWNED` / `SPAWNFAIL`). `edenmatch` does not
implement the agent role — it spawns locally under `--allow-host` instead.

## `edenmatch` flags

| Flag | Default | Meaning |
|---|---|---|
| `--port N` | `27020` | TCP port to listen on (all interfaces). A bare leading number also works. |
| `--advertise-ip IP` | *(peer address)* | Override the advertised address for **every** registration — the address clients are told to dial. Without it, each server's row uses the address it sent in `REGISTER`, or its peer IP if it sent none. |
| `--short-list` | off | Emit the 4-field `SERVER:` row instead of the 7-field capture grammar. |
| `--verbose` | off | Log every `LIST`, `HOST`, bad line, and unknown verb, not just registrations. |
| `--allow-host` | off | Honour `HOST:` by spawning a local `edenserver`. |
| `--edenserver PATH` | `./edenserver` | Binary to spawn for `HOST` (with `--allow-host`). |
| `--world DIR` | `.` | Working directory for a spawned server (world/player/sign files land here). |
| `--host-ports LO-HI` | `27600-27699` | Port range `HOST` allocates from. |

Registrations are held in memory only; `edenmatch` keeps no state on disk and needs no
configuration file. Restarting it drops every registration until each server reconnects (which
`ewb-server` does automatically within a few seconds).

## Running it

```
./build_server.sh          # builds ./edenmatch alongside ./edenserver
./edenmatch --verbose       # listen on 27020

# in another shell / on another host:
./edenserver --port 27015 --name "My World" --matchmaker 127.0.0.1:27020 --advertise 192.168.1.20
```

Firewall: open TCP **27020** only if you are hosting a matchmaker other machines must reach.
See [`ops/INSTALL.md`](../ops/INSTALL.md).
