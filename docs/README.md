# ewb-server documentation

**Audience:** everyone — start here, then follow the pointer for your job.

This directory is the shipped reference for `ewb-server`, a standalone C++ multiplayer server
for **Eden** (Eden World Builder). It speaks the game's native wire protocol, so an unmodified
retail client can connect to a world you host yourself.

## Who should read what

| You are… | Read, in order |
|---|---|
| **Hosting a server** | [quickstart.md](quickstart.md) → [configuration.md](configuration.md) → [commands.md](commands.md) |
| **Hosting a world you already have** | [import.md](import.md) → [quickstart.md](quickstart.md) |
| **Taking a hosted world back offline to edit** | [export.md](export.md) → [import.md](import.md) |
| **Debugging a connection** | [protocol.md](protocol.md) → [configuration.md](configuration.md) |
| **Changing the code (human or agent)** | [architecture.md](architecture.md) → [protocol.md](protocol.md) → [configuration.md](configuration.md) |

- [quickstart.md](quickstart.md) — build it, run it, connect a retail client. Also states
  plainly what is **not** covered yet.
- [architecture.md](architecture.md) — how the server is put together: translation unit +
  pure headers, threading, the world model, persistence, the test convention.
- [protocol.md](protocol.md) — every wire message in both directions, the join sequence, the
  coordinate model.
- [configuration.md](configuration.md) — exhaustive reference for command-line flags,
  environment variables, and on-disk file formats.
- [matchmaker.md](matchmaker.md) — the matchmaker wire protocol and the standalone `edenmatch`
  server: how a server gets listed in the in-game Server Browser.
- [import.md](import.md) — converting a saved `.eden` world into one this server can host:
  the `eden_import` tool, the base terrain profile, the three fill strategies, and the two
  budget numbers that decide whether an imported world can actually be played.
- [export.md](export.md) — the other direction: converting a world this server hosts back into a
  `.eden` with the `eden_export` tool, what the round trip preserves, what it loses, and the
  summary and exit codes a script can rely on.
- [commands.md](commands.md) — both command surfaces: the operator control socket and `edenctl`
  (kick/ban, live world edits, save/stop, sign editing, `region-stats`), and the player commands
  typed into game chat (`//set`, `//sphere`, `/tp`, …) with their permission levels and bounds.

An operations guide (`operations.md`: service supervision, firewall, backups) is not written
yet; see [quickstart.md § What this does not cover](quickstart.md#what-this-does-not-cover).

## Keep these files updated

**`docs/` is the live source of truth for how this server works, and it is updated in the same
commit as the code change that makes it true.** Not a follow-up commit, not a TODO. This is the
same rule that already governs the (untracked) roadmap: a change that lands without its doc
edit is an incomplete change.

Practically, when you touch a source file, move the doc in the same row:

| When you change… | Update |
|---|---|
| `server_posix.cpp` — command-line flag parsing in `main()` | [configuration.md](configuration.md) |
| `server_posix.cpp` — `matchmakerThread()`, or `matchmaker.h` / `edenmatch.cpp` (the matchmaker) | [matchmaker.md](matchmaker.md) |
| `server_posix.cpp` — the control socket (`handleControlLine()`), `control.h`, `edenctl` | [commands.md](commands.md); [configuration.md](configuration.md) for the flags and file formats |
| `server_posix.cpp` — the player commands (`handleWorldEditLine()`), `worldedit.h` | [commands.md](commands.md) — including its **reply-string contract**, which tools parse; [configuration.md](configuration.md) for the flags |
| `worldedit.h` — `we_emit_edit_wire()`, the `ACTION:server:0:…` relay shape (player commands and `setblock`/`fill`) | [protocol.md](protocol.md) § Legacy world snapshot (the relay rules); [commands.md](commands.md) |
| `base_profile.h` — the base terrain profile (what an untouched cell looks like) | [import.md](import.md) § The base terrain profile; [commands.md](commands.md) § Untouched ground |
| `eden_names.h` — community block / paint / character name tables (`/id`, `/searchblocks`, `/searchcolors`, named `//set` / `//paint`) | [commands.md](commands.md) |
| `server_posix.cpp` — any message handled in `handleClient()`, or anything the server sends | [protocol.md](protocol.md) |
| `server_posix.cpp` — join sequence, threading, world model, persistence/autosave | [architecture.md](architecture.md), and [protocol.md](protocol.md) if the join order moves |
| `region_query.h`, `snapz_codec.h` — `REGION`/`SNAPZ` geometry, encoding or framing | [protocol.md](protocol.md) |
| `world_store.h` — the chunk store, the mined sentinel, the `EDMB` world format, the legacy text reader | [configuration.md](configuration.md) § `eden_world.model` (both formats + `--world-format`); [architecture.md](architecture.md) § World model / Persistence |
| `explode.h` — TNT/paint explosion chain, `--burn-max-cells` work budget, the `protectedAt` zone hook | [protocol.md](protocol.md) (`ACTION` mode 2 bounds; § Protected zones for the hook), [configuration.md](configuration.md) (the compiled-in-limits table) |
| `zones.h` — the `eden_zones.txt` grammar and zone lookups | [configuration.md](configuration.md) § `eden_zones.txt` |
| `zone_guard.h`, or `zoneDenies()` and its callers in `server_posix.cpp` — protected-zone enforcement: the restore wire, the revert queue, notices, audit folding | [protocol.md](protocol.md) § Protected zones; [configuration.md](configuration.md) § Protected zones, the audit channel and the compiled-in limits; [architecture.md](architecture.md) (threads, locks, world model) |
| `auth.h`, or `handleLogin()` / the `passwd`/`unpasswd`/`pins` verbs in `server_posix.cpp` — PINs, `/login`, the level a session gets, zone bypass | [commands.md](commands.md) § Player identity (and its reply strings); [configuration.md](configuration.md) § `eden_auth.txt`, `--auth-file`, `eden_ops.txt`, Protected zones; [protocol.md](protocol.md) § Chat and § Protected zones; [architecture.md](architecture.md) § Locks |
| `topmap.h`, or the `topmap` verb | [commands.md](commands.md) (the verb and its reply format) |
| `durable_write.h` — how the world and sidecar files reach disk (`fsync` + `rename` + directory `fsync`) | [architecture.md](architecture.md) § Persistence (the durability guarantee); [configuration.md](configuration.md) § Temporary files |
| `out_queue.h`, or anything that writes to a player's socket | [architecture.md](architecture.md) § The output path; [configuration.md](configuration.md) for the per-client bounds |
| `sign_store.h` — `SIGNQ`/`SIGNP` wire shape | [protocol.md](protocol.md); the `eden_signs.txt` grammar lives in [configuration.md](configuration.md) |
| `spawn_store.h` — `eden_spawn.txt` grammar (world default spawn) | [configuration.md](configuration.md) § `eden_spawn.txt` and the `--spawn` / `--spawn-file` flags |
| `motd_store.h` — `eden_motd.txt` grammar (the join welcome message) | [configuration.md](configuration.md) § `eden_motd.txt` and `--motd-file`; [protocol.md](protocol.md) § Join sequence; [commands.md](commands.md) for `motd reload` |
| `hardening.h` — limits, username rules, `ACTION` and movement validation, the pre-`JOIN` verb gate | [protocol.md](protocol.md) (what clients may send, and when) and [configuration.md](configuration.md) (what an operator can tune) |
| `build_server.sh` — compiler, flags, dependencies, or the test suites it runs | [quickstart.md](quickstart.md), [architecture.md](architecture.md) |
| `admin/` — the optional `edenadmin` operator GUI (separate Go module) | `admin/README.md` (not part of `docs/`); mention it in [quickstart.md](quickstart.md) / [architecture.md](architecture.md) only where it touches the build |
| `phase3_live_test.py`, `phase7_live_test.py`, `phase8_live_test.py` — the by-hand live socket tests | [architecture.md](architecture.md) (what each one covers) |
| `server_posix.cpp` — anything written to the audit channel (`auditLog()`) | [configuration.md](configuration.md) § The audit channel |
| `host_world.sh`, `run_server.bat` — launcher arguments | [configuration.md](configuration.md), [quickstart.md](quickstart.md) |
| `ops/edenserver.service`, `ops/edenserver@.service`, `ops/edenserver.conf.example`, `ops/edenserver-writeconf` — the systemd `EnvironmentFile` layer (`EDEN_*` keys, `${VAR}` vs `$VAR`) | [configuration.md](configuration.md) § The systemd EnvironmentFile; `ops/INSTALL.md` (template unit in § Multi-world hosting) |
| `ops/edenserverctl` verbs, `ops/edenserver-backup.{service,timer}` | [configuration.md](configuration.md) § Ops wrapper; `ops/INSTALL.md` |
| `eden_file.h` — `.eden` world-format parsing (header, chunks, spans, signs, ZIP wrapper) | [import.md](import.md) § What it reads; [architecture.md](architecture.md) (file map) |
| `eden_import.h` / `eden_import.cpp` — the converter: base profile, fill strategies, axis rename, budget projections, the `eden_origin.txt` grammar, flags | [import.md](import.md); [export.md](export.md#the-origin-sidecar) for the origin keys; [configuration.md](configuration.md) for the files it writes |
| `eden_export.h` / `eden_export.cpp` — the `.eden` **writer**: chunk selection, fill-then-overlay, the cell sentinels, the axis rename backwards, height format, signs sidecar vs inline, origin replay, flags | [export.md](export.md); [import.md](import.md) if the round trip's shape changes |
| A new header + `*_test.cpp` pair | [architecture.md](architecture.md) (the file map) and whichever doc above owns its behaviour |
| On-disk format: `eden_world.model`, `eden_players.txt`, `eden_signs.txt`, `eden_spawn.txt`, `eden_bans.txt`, `eden_ops.txt`, `eden_zones.txt` | [configuration.md](configuration.md) |

Two more rules for anything written here:

- **Describe only what exists.** These docs must never present unbuilt work as shipped. Link to
  the roadmap for "what's next" instead.
- **This repository is public.** No secrets, no tokens, no real player data, and no private
  reverse-engineering material. Cite a protocol *conclusion*; do not reproduce capture data.
