# Commands

**Audience:** anyone running a server, and anyone documenting what players can type.

`ewb-server` has two command surfaces, and the split is deliberate:

| | **Tier 1 — operator** | **Tier 2 — player** |
|---|---|---|
| Where | A local unix socket, never the game wire | Chat lines, over the game wire |
| Who | Whoever can open the socket file | Anyone who completed a `JOIN` |
| Authentication | Filesystem permissions | None — so **permission levels** gate every verb |
| Reference | [Part 1](#part-1--operator-control-socket) | [Part 2](#part-2--player-commands) |

Tier 1 invents no protocol a client has to understand and there is nothing in it a connected
player can reach. Tier 2 is the opposite: every byte arrives from an untrusted peer, so it is
bounded and permissioned throughout.

## Part 1 — operator control socket

## Transport and authentication

- A unix domain socket, mode `0600`, owned by the user the server runs as.
- Default path: `<world dir>/edenserver.sock` (next to `--world`). Override with
  `--control-socket PATH`; disable entirely with `--no-control-socket`.
- **Filesystem permissions are the authentication.** There is no password and no network
  address — a UDS cannot be misconfigured into being remote. Keep it inside the service user's
  directory (under systemd, the unit's `WorkingDirectory` already is the world directory).
- Line grammar: `verb[:rest]`. The verb runs to the first `:`; everything after is a single
  remainder the command splits as it needs, so a `say` message or a sign's text keeps its own
  `:` and spaces.

## `edenctl`

```
./edenctl [-S <socket>] <command> [args...]
```

`edenctl` joins the command's arguments with `:` to build the wire line — you pass normal
shell words:

```
edenctl say "server restarting in 5 minutes"
edenctl kick Griefer "spamming chat"
edenctl fill 65530 33 65530 65542 33 65542 3 14
```

Socket path: `-S`, else `$EDENSERVER_CONTROL_SOCKET`, else `./edenserver.sock`, else
`$EDENSERVER_WORLD_DIR/edenserver.sock`, else `/var/lib/edenserver/world/edenserver.sock`. The
host needs one of `nc` (with `-U`), `socat`, or `python3`.

A raw client works too — `printf 'who\n' | nc -U ./edenserver.sock`.

## Commands

| Command | What it does |
|---|---|
| `help` | List every command. |
| `who` | Connected players: name, character type, IP, position, op level. |
| `say <text>` | Broadcast a `[Server] <text>` chat line to everyone. |
| `kick <name> [reason]` | Disconnect a player by exact name. They see `[Server] You were <reason>.` |
| `ban <name\|ip>` | Add to the ban list, persist it, and disconnect anyone matching now. A token of only digits/dots/colons is an IP; anything else is a name. |
| `unban <name\|ip>` | Remove a ban-list entry. |
| `banlist` | Show the ban list. |
| `save` | Flush the world and player files to disk now (independent of the 15 s autosave). |
| `stop` | Save, then exit the process. Under a restart-on-failure supervisor this is a clean stop, not a crash. |
| `op <name> <0..2>` | Set a player's op level (`0` visitor / `1` builder / `2` operator). Persisted to `eden_ops.txt`, and enforced on the player's **next command** — no reconnect needed. See [Part 2](#part-2--player-commands). |
| `deop <name>` | Clear a player's op-level entry. |
| `setblock <x> <y> <z> <type> [color]` | Place one block. Goes through the same world model + broadcast path as a player edit, so connected clients see it immediately. |
| `fill <x0> <y0> <z0> <x1> <y1> <z1> <type> [color]` | Fill an inclusive box. Capped at `2 x --we-max-cells` (262,144 by default); an oversized box is refused with its size, not truncated. `type 0` clears to air. |
| `signs reload` | Re-read the sign sidecar from disk. |
| `signs add <x> <y> <z> <a> <b> <c> <text>` | Append a sign and rewrite the sidecar. `a`/`b`/`c` are of unknown meaning — `0 0 0` is a fine default. New `SIGNQ`s get the updated burst. |
| `signs rm <x> <y> <z>` | Remove every sign at that coordinate and rewrite the sidecar. |
| `region-stats` | `REGION` service counters since start: requests, cells scanned, records emitted, bytes out, total and mean time. |

### Notes

- **Coordinates** use the same model as the wire (see [protocol.md](protocol.md)): `x`/`z` in
  `0..16777215` centred on 65536, `y` the height in `0..255`. `type` is `0..127` (0 = air),
  `color` is `0..54` (0 = unpainted). Out-of-range arguments are rejected with a message.
- **Admin edits are authoritative and broadcast.** `setblock` and `fill` change the server's
  world model and relay `ACTION:server:…` to every connected client, so nobody has to re-`REGION`
  to see them.
- **`ban` enforcement:** IP bans are checked at `accept()` before a connection thread starts;
  name bans are checked at `JOIN`. Both are read from `eden_bans.txt` at startup.
- Every command that changes state is written to the audit channel — one `[Audit]` line with a
  UTC timestamp, on stdout and in `--audit-file`. See
  [configuration.md § The audit channel](configuration.md#the-audit-channel).
- **`fill`'s cap is `2 x --we-max-cells`** (262,144 by default), capped at the world's
  edited-cell ceiling. It is derived from the player tier's cap rather than set separately
  because both bound the same thing: one pass over the world holding the world lock. Moving
  `--we-max-cells` moves both. A `fill` past the cap is refused with its size and the flag name.
- **The socket is paced.** 64 commands at once refilling at 16/s per connection, 8 concurrent
  connections, disconnect after 8 refusals, 300 s idle timeout. Filesystem permissions decide who
  may connect; these decide how fast, so a runaway script cannot monopolise the world lock. All
  three are tunable — see [configuration.md](configuration.md).

### What is not here yet

- Per-command permission levels *within* Tier 1. Anyone who can open the socket can run every
  verb; the tier is all-or-nothing by design, because filesystem permissions are the whole
  authentication model.
- A `getSelection`-style persistent operator selection, so `fill` still takes six coordinates.

---

## Part 2 — player commands

Players type these into the game's chat box. The client sends them as ordinary chat, so no
client modification is needed; the server recognises a leading `/` and answers on the same
connection instead of relaying the line. **A command is never broadcast** — otherwise one
player's `/msg` text would reach the whole server.

The vocabulary follows WorldEdit and Minecraft closely on purpose: if you have used either, you
already know it.

### Permission levels

Every command has a minimum level. A player's level comes from `eden_ops.txt` (see
[configuration.md](configuration.md#eden_opstxt)), or from `--default-level` if they have no
entry. Set one with the operator socket: `edenctl op Alice 1`.

| Level | Name | Can do |
|---|---|---|
| `0` | visitor | Read-only and self-scoped: help, lookups, whispers, `/resync`. **The default.** |
| `1` | builder | Everything above, plus every `//` build command and `/tp` to a coordinate. |
| `2` | operator | Everything above, plus `/tp <player>` — which reveals that player's exact position. |

`/help` lists only what the caller may actually run, so a visitor is never shown a command they
would be refused. The level is re-read on every command: an `op` or `deop` from the control
socket takes effect on the player's next line, not their next session.

An **open creative server** hands everyone building rights with `--default-level 1`. That is a
deliberate choice, not a default — see [Bounds](#bounds) for what it does and does not cost you.

### Selection

`//pos1` and `//pos2` mark two opposite corners at your **feet**, and every box command works on
the inclusive box between them. Setting the second corner reports the box's size, so you find out
a selection is too large before you type the command that would be refused.

| Command | Level | What it does |
|---|---|---|
| `//pos1` | 1 | First corner, at your feet. |
| `//pos2` | 1 | Second corner. Reports the resulting cell count. |

### Building

| Command | Level | What it does |
|---|---|---|
| `//set <block> [color]` | 1 | Fill the selection. `//set 0` clears it to air. |
| `//walls <block> [color]` | 1 | The selection's four vertical sides only. |
| `//paint <color>` | 1 | Recolour the selection. Untouched natural cells become painted base. |
| `//unpaint` (`//strip`) | 1 | Strip paint back to unpainted. |
| `//replace <old> <new> [color] [oldColor]` | 1 | Swap one block for another inside the selection. `oldColor` filters by existing paint. |
| `//replacenear <radius> <old> <new> [color] [oldColor]` | 1 | The same, in a sphere around you, no selection needed. |
| `//sphere <radius> <block> [color]` | 1 | Solid ball centred on your feet. |
| `//hsphere <radius> <block> [color]` | 1 | Hollow shell. |
| `//cyl <radius> <height> <block> [color]` | 1 | Solid cylinder rising from your feet. |
| `//hcyl <radius> <height> <block> [color]` | 1 | Hollow cylinder wall. |
| `//up <dist>` | 1 | Rise `dist` blocks, standing on a block the server places under you. Undoable like any other edit. |

`//replace` and `//replacenear` match **player-made cells only**. The server stores edits over
terrain every client generates identically, so it genuinely does not know what natural block sits
in an untouched cell — and will not pretend to. `//set`, `//sphere` and friends are unconditional
and affect untouched cells normally.

### Clipboard and history

| Command | Level | What it does |
|---|---|---|
| `//copy` | 1 | Copy the selection's player-made blocks, relative to your feet. |
| `//paste` | 1 | Paste the clipboard at your feet. |
| `//rotate <90\|180\|270>` | 1 | Rotate the clipboard about your own column. |
| `//undo` | 1 | Reverse your last edit. |
| `//redo` | 1 | Reapply what you undid. A new edit discards the redo stack. |

> **Ramp facing under `//rotate` is not yet verified.** Positions rotate exactly; the block-id
> remap that turns a ramp to face its new direction is inherited from a community
> implementation that disagrees with the world editor's, and no capture settles which is
> correct. A rotated ramp may face the wrong way. Tracked in the roadmap.

**Undo restores air, not natural terrain.** If you build on an untouched cell and undo it, the
cell becomes air rather than the natural block that was there. The wire protocol has no "revert
to base terrain" message, so restoring it in the model without being able to say so to connected
clients would desync them permanently. Air is the answer both the model and every client agree
on.

### Movement, chat and lookups

| Command | Level | What it does |
|---|---|---|
| `/help [page]` | 0 | List the commands you can run, a page at a time. |
| `/msg <player> <text>` | 0 | Private message. |
| `/r <text>` | 0 | Reply to whoever whispered to you last. |
| `/tp <x> <y> <z>` | 1 | Teleport. `~` is your current value, `~5` an offset from it. |
| `/tp <player>` | **2** | Teleport to a player — this discloses their exact position, so it is operator-only. |
| `/resync` | 0 | Resend the world around you, if your client has drifted. Paced by the same budget as a client's own `REGION` requests. |
| `/id <block\|colour>` | 0 | Look up an id: a number reports what it names, a name reports its number. |
| `/searchblocks <name>` | 0 | List blocks whose name contains the text, as `name=id`. |
| `/searchcolors <name>` | 0 | List colours whose name contains the text, as `name=id`. |

> **Blocks and colours can be named or numbered.** `//set 2`, `//set stone`, `//paint red` and
> `//paint 19` are all accepted; `/searchblocks slope` and `/id stone` help you find the word.
> Names come from a community reverse-engineered table (`eden_names.h`) and are **not
> authoritative** — a few blocks are named differently by the companion editor, and blocks 24–27
> / 40–55 carry a known disagreement about which way the ramp faces (see the `//rotate` note
> above). Numbers are the ground truth: blocks `0..127` (`0` = air), colours `0..54`
> (`0` = unpainted), the same ranges the wire accepts. An unknown name is refused, never guessed.

### Bounds

These exist so that one chat line from one player cannot stall the server for everyone else.
They are tunable ([configuration.md](configuration.md)); they are not removable.

| Bound | Default | Flag |
|---|---|---|
| Cells one command may read or write | 131,072 | `--we-max-cells` |
| Cells per second per player | 32,768 (burst 262,144) | `--we-rate`, `--we-burst` |
| Undo + redo memory per player | 2 MiB | `--we-undo-budget` |
| Whole tier off | on | `--no-worldedit` |

- **Selections are capped at the moment they are read**, not per command. A command added later
  cannot forget the cap, because there is no unbounded way to read a selection.
- **The rate limit counts cells, not commands.** One `//sphere 40 2` is a single command and
  roughly 268,000 cells; a per-command limiter would not be a limiter.
- **Radius-driven shapes are capped by the box they imply**, so `//sphere 400 2` is refused with
  its size, the same as an oversized selection.
- An edit that would push the world past its edited-cell ceiling is **refused, not truncated**.
- **Every command that changes cells is audited**, at every level, whether or not `--verbose` is
  on — see [configuration.md § The audit channel](configuration.md#the-audit-channel).
- **A large edit does not hold the world lock while it talks.** The batch is decided and written
  under the lock; the `ACTION` relay is formatted and sent after it is released. Building the
  relay is the larger half of the work, so this is most of the stall (measured: ~51 ms → ~13 ms
  for a full-cap 131,072-cell edit).
