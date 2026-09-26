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
edenctl motd reload
```

Socket path: `-S`, else `$EDENSERVER_CONTROL_SOCKET`, else `./edenserver.sock`, else
`$EDENSERVER_WORLD_DIR/edenserver.sock`, else `/var/lib/edenserver/world/edenserver.sock`. The
host needs one of `nc` (with `-U`), `socat`, or `python3`.

A raw client works too — `printf 'who\n' | nc -U ./edenserver.sock`.

## Commands

| Command | What it does |
|---|---|
| `help` | List every command. |
| `who` | Connected players: name, character type, IP, position, the op level the session **actually has** (see [Player identity](#player-identity-pins-and-login)), their output backlog — bytes queued now, the peak for the session, `REGION` replies still in flight, and any stale movement/chat lines dropped — and, last on the row, `auth verified` (logged in with a PIN), `auth unverified` (the name has a PIN, not logged in) or `auth none` (no PIN). A client with a persistently large backlog is on a weak link. |
| `say <text>` | Broadcast a `[Server] <text>` chat line to everyone. |
| `kick <name> [reason]` | Disconnect a player by exact name. They see `[Server] You were <reason>.` |
| `ban <name\|ip>` | Add to the ban list, persist it, and disconnect anyone matching now. A token of only digits/dots/colons is an IP; anything else is a name. |
| `unban <name\|ip>` | Remove a ban-list entry. |
| `banlist` | Show the ban list. |
| `save` | Flush the world, player and sign files to disk now (independent of the 15 s autosave). |
| `stop` | Save, then exit the process. Under a restart-on-failure supervisor this is a clean stop, not a crash. |
| `op <name> <0..2>` | Set a player's op level (`0` visitor / `1` builder / `2` operator). Persisted to `eden_ops.txt`, and enforced on the player's **next command** — no reconnect needed. See [Part 2](#part-2--player-commands). |
| `deop <name>` | Clear a player's op-level entry. |
| `setblock <x> <y> <z> <type> [color]` | Place one block. Goes through the same world model + broadcast path as a player edit, so connected clients see it immediately. |
| `fill <x0> <y0> <z0> <x1> <y1> <z1> <type> [color]` | Fill an inclusive box. Capped at `2 x --we-max-cells` (262,144 by default); an oversized box is refused with its size, not truncated. `type 0` clears to air. |
| `signs reload` | Re-read the sign sidecar from disk, then drop any sign on a block the world stores as air and rewrite the file (the reply says how many). Refused while players have placed or removed signs that are not saved yet — run `save` first. A running server's saves rewrite the file, so hand-edit it only while the server is stopped. |
| `signs add <x> <y> <z> <a> <b> <c> <text>` | Add a sign and rewrite the sidecar. A sign for a block face that already has one **replaces** it (the same rule as a player's sign write) rather than adding a duplicate, and past the 20,000-sign cap it is refused with `error: sign cap reached`. `a`/`b`/`c` are of unknown meaning — `0 0 0` is a fine default. New `SIGNQ`s get the updated burst. |
| `signs rm <x> <y> <z>` | Remove every sign on that block, whatever its face, and rewrite the sidecar. You rarely need it for a sign whose block is gone: any edit that turns a block to air removes its signs. |
| `motd reload` | Re-read the welcome-message sidecar (`eden_motd.txt` / `--motd-file`) from disk. Takes effect on the next player to join; nobody is disconnected and nothing else is touched. Unlike `signs reload` it can never refuse: the server never writes this file, so there is no unsaved state to protect. |
| `motd show` | Print the MOTD that is live right now, as the wire lines a joining player is sent. |
| `region-stats` | `REGION` service counters since start: requests, cells scanned, records emitted, bytes out, and total/mean time (scan and sort only — encoding and sending happen on each client's writer thread and are reported per reply in the `REGION drain` log line). Then backpressure: requests refused because a client's queue or the server's record backlog was full, records queued right now against `--region-pending-records`, and clients disconnected for falling too far behind on world updates. |
| `zones` | List protected zones: name, bounds, flags (`all`/`off`), bypass level (`-` if none set), and cell count. |
| `zone add <name> <x0> <y0> <z0> <x1> <y1> <z1> [all\|off] [level]` | Create a new protected zone. Either corner order is accepted and normalised. Flags default to `all` (enforced) if omitted. Refused on a duplicate name, an unrecognised flag, out-of-range coordinates, or the 256-zone cap. |
| `zone set <name> <x0> <y0> <z0> <x1> <y1> <z1>` | Resize or move an existing zone, keeping its flags and level. Refused if the name doesn't exist. |
| `zone flags <name> <all\|off> [level]` | Change a zone's enforcement without touching its bounds. Omitting `level` keeps whatever was already set. A `level` (`0..2`) lets a player **logged in with a PIN** at that level or above build inside; the reply says who that is, and warns when no name has a PIN yet (so nobody can). |
| `zone rm <name>` | Delete a zone. Refused if the name doesn't exist. |
| `zone reload` | Re-read `eden_zones.txt` from disk (for a hand-edited file), same all-or-nothing rule as startup: a malformed file is refused and the in-memory set is left untouched. |
| `topmap <x0> <z0> <x1> <z1> <step>` | A top-down map of the surface: the box is sampled every `step` blocks (at most 256 × 256 samples; a bigger request is refused with the smallest `step` that fits). Reply format below. Read-only; the world lock is taken one 16 × 16 chunk column at a time. |
| `passwd <name>` | Issue a login PIN for a player name, or replace its PIN. The 8-digit PIN is in the reply and **nowhere else** — it is stored only as a salted hash in `eden_auth.txt`, never logged or audited — so hand it to the player now. A live session under that name is logged out. See [Player identity](#player-identity-pins-and-login). |
| `unpasswd <name>` | Remove a name's PIN. The name goes back to being claimed by name alone; a live session under it is logged out. |
| `pins` | Names that have a PIN, each `offline`, `online verified` or `online unverified`. |

### Notes

- **Coordinates** use the same model as the wire (see [protocol.md](protocol.md)): `x`/`z` in
  `0..16777215` centred on 65536, `y` the height in `0..255`. `type` is `0..127` (0 = air),
  `color` is `0..54` (0 = unpainted). Out-of-range arguments are rejected with a message.
- **Admin edits are authoritative and broadcast.** `setblock` and `fill` change the server's
  world model and relay `ACTION:server:…` to every connected client, so nobody has to re-`REGION`
  to see them.
- **`ban` enforcement:** IP bans are checked at `accept()` before a connection thread starts;
  name bans are checked at `JOIN`. Both are read from `eden_bans.txt` at startup.
- **Changing the welcome message** is edit-then-reload: write the lines into the world's
  `eden_motd.txt` (grammar in
  [configuration.md § `eden_motd.txt`](configuration.md#eden_motdtxt)) and run `motd reload`.
  The file is operator input only — the server never rewrites it — so editing it on a running
  server is safe, unlike `eden_signs.txt`.
- Every command that changes state is written to the audit channel — one `[Audit]` line with a
  UTC timestamp, on stdout and in `--audit-file`. See
  [configuration.md § The audit channel](configuration.md#the-audit-channel).
- **`fill`'s cap is `2 x --we-max-cells`** (262,144 by default), capped at the world's
  edited-cell ceiling. It is derived from the player tier's cap rather than set separately
  because both bound the same thing: one pass over the world holding the world lock. Moving
  `--we-max-cells` moves both. A `fill` past the cap is refused with its size and the flag name.
  Separately, at `--max-world-cells` a `fill` / `setblock` only stores cells the world already
  holds or has room for: cells the cap refuses are **not relayed to players** and are reported in
  the reply (`ok: filled 10 cells, but refused 6 cell(s): …` when partly applied, `error: …` when
  nothing landed; `setblock` is an `error:`) and in the audit line (`(refused N at the world cell cap)`).
- **The socket is paced.** 64 commands at once refilling at 16/s per connection, 8 concurrent
  connections, disconnect after 8 refusals, 300 s idle timeout. Filesystem permissions decide who
  may connect; these decide how fast, so a runaway script cannot monopolise the world lock. All
  three are tunable — see [configuration.md](configuration.md).
- **`topmap`'s reply** is one header line then one line per z row (north to south), each with one
  token per sample (west to east), space-separated:

  ```
  ok: topmap <x0> <z0> <x1> <z1> <step> <W> <H>
  - - 33,5,0 32,8,12 31,3,0 .
  ```

  `-` is an untouched column (the natural surface — grass at y 32). `y,type,color` is the
  highest solid block a player sees there, counting the natural terrain under anything mined
  away (a mined grass block reads as the dirt beneath it); a painted natural block is reported
  as its natural type with its paint colour. `.` is a column dug out to nothing. Samples are
  single columns (`x0 + i·step`, `z0 + j·step`), not an average over the step, so the cost is the
  same at every zoom; `x1`/`z1` in the header are the last *sampled* coordinates. Measured on a
  13.9 M-cell world: 7–11 ms for a full 256 × 256 reply at any step, with no single world-lock
  hold above ~30 µs.
- **Zone edits take effect on the next player edit, no restart.** Each `zone add`/`set`/`flags`/
  `rm` reads the current set, applies the one change, atomically rewrites `eden_zones.txt`
  (`durable_write.h`), then swaps the live set in — the same swap `zone reload` does. Protected
  boxes are enforced against every player build/mine/paint/burn/sign edit; see
  [protocol.md](protocol.md) and [architecture.md](architecture.md) for the enforcement path. The
  control socket itself is never subject to a zone.

### Player identity: PINs and `/login`

A player's name is whatever their client sends in `JOIN`, and the world password is shared, so a
name alone proves nothing. **An `eden_ops.txt` level on a name with no PIN goes to anyone who joins
under that name** while its owner is offline.

A PIN closes that, one name at a time:

1. `edenctl passwd Alice` prints an 8-digit PIN once. Give it to Alice privately.
2. Alice joins; the server tells her the name is protected. Until she types `/login <pin>` in
   chat she has `--default-level` (or her own level, if that is lower) — so does anyone else who
   joins as `Alice`.
3. After `/login` she has her `eden_ops.txt` level, and may build inside zones whose bypass level
   she meets. `who` shows `auth verified`.

Only a **logged-in** session bypasses a zone, and only a logged-in level-2 player may use
[`/zone`](#protected-zones-zone). A name with no PIN behaves exactly as it did before PINs
existed, apart from never bypassing a zone. Give every operator a PIN.

What a PIN is not: the game's wire is plaintext, so a PIN is as secret as the world password —
it stops someone borrowing a name, not someone who can read the player's traffic. `eden_auth.txt`
holds only salted PBKDF2-HMAC-SHA-256 hashes, but an 8-digit space is small, so the file is written
`0600` and should be guarded like the password file.

Wrong PINs are paced: three attempts per connection at once, then one per 10 s; and five wrong
PINs from one address inside a minute lock that address out of `/login` for a minute, doubling on
repeat up to an hour (`--auth-fail-limit` sets the count for both this and the `JOIN` password;
0 turns the per-address lockout off). The lockout is an audit line. A malformed PIN is refused
without counting. The PIN never appears in the server's output or the audit channel.

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

> ⚠️ **A level is keyed on the name, and a name is only a claim.** Anyone joining as `Alice`
> while Alice is offline gets Alice's level — unless the name has a PIN, in which case only a
> session that has typed `/login <pin>` does. Give every name above level 0 a PIN
> (`edenctl passwd <name>`; see
> [Player identity](#player-identity-pins-and-login)).

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

`//pos1` and `//pos2` mark two opposite corners, at your **feet** or at a coordinate you give,
and every box command works on the inclusive box between them. Setting a corner once both are set
reports the box's size, so you find out a selection is too large before you type the command
that would be refused.

| Command | Level | What it does |
|---|---|---|
| `//pos1 [x y z]` | 1 | First corner: your feet, or the cell you name. |
| `//pos2 [x y z]` | 1 | Second corner. Reports the resulting cell count. |

A corner takes **all three** coordinates or none. Each coordinate is the `/tp` grammar: a number
is absolute, `~` is your feet's value on that axis, `~5` / `~-5` an offset from it, and a fraction
rounds to the nearest cell. So `//pos1 ~ ~ ~` is exactly `//pos1`, and `//pos2 ~10 ~ ~10` is ten
cells out on both horizontal axes. A corner given entirely in numbers needs no position from you
at all, so a tool can set a selection anywhere without walking there first. A corner outside the
world (`y` outside `0..255`, `x`/`z` outside `0..16777215`) is refused.

### Building

| Command | Level | What it does |
|---|---|---|
| `//set <block> [color]` | 1 | Fill the selection. `//set 0` clears it to air. |
| `//walls <block> [color]` | 1 | The selection's four vertical sides only. |
| `//paint <color>` | 1 | Recolour every block in the selection, natural ground included. Air — open sky or carved out — has nothing to paint and is skipped. |
| `//unpaint` (`//strip`) | 1 | Strip paint back to unpainted. |
| `//replace <old> <new> [color] [oldColor]` | 1 | Swap one block for another inside the selection. `oldColor` filters by existing paint. |
| `//replacenear <radius> <old> <new> [color] [oldColor]` | 1 | The same, in a sphere around you, no selection needed. |
| `//sphere <radius> <block> [color]` | 1 | Solid ball centred on your feet. |
| `//hsphere <radius> <block> [color]` | 1 | Hollow shell. |
| `//cyl <radius> <height> <block> [color]` | 1 | Solid cylinder rising from your feet. |
| `//hcyl <radius> <height> <block> [color]` | 1 | Hollow cylinder wall. |
| `//up <dist>` | 1 | Rise `dist` blocks, standing on a block the server places under you. Undoable like any other edit. |

`//replace` and `//replacenear` match **player-made cells only** — natural ground is never
matched, so `//replace 8 3` swaps grass a player placed and leaves the landscape alone. `//set`,
`//sphere` and friends are unconditional and affect untouched cells normally.

#### Untouched ground

The server stores edits over terrain every client draws for itself: bedrock at `y = 0`, stone
`1..15`, dirt `16..31`, grass at `32`, air above (the table and how it was measured are in
[import.md](import.md#the-base-terrain-profile)). A cell nobody has edited is read as that
natural block, which has three consequences:

- **An edit that changes nothing is not an edit.** `//set 0` over open sky, `//set 2` over natural
  stone and `//unpaint` on bare ground store nothing, relay nothing, and are not counted in the
  reply. They also cost nothing against the world's edited-cell limit.
- **The edited-cell limit counts new cells, not the box.** Only cells the world does not already
  store can push it over the limit, so on a nearly full world you can still rework what is already
  built. An edit that would genuinely add too many new cells is refused whole, never truncated.
- **`//undo` puts natural ground back.** Build over grass and undo it, and the grass returns —
  sent to every client as an ordinary build, so everyone sees the same thing.

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

**Undo restores what was there, natural terrain included.** If you build on an untouched cell and
undo it, the natural block for that height comes back (open sky comes back as air). The server
stores and relays it as an explicit block rather than "forgetting" the cell: the wire has no
"revert to base terrain" message, so an explicit block is the only answer the model and every
connected client can agree on. The cell therefore stays in the world's edited-cell count after an
undo.

`//copy` copies what you see: a natural block you painted is copied as that block, painted, so
it pastes correctly at any height.

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
| `/login <pin>` | 0 | Prove this name is yours with the PIN the operator issued; you then get its level. See [Player identity](#player-identity-pins-and-login). |

### Protected zones (`/zone`)

Level 2 **and** logged in with a PIN — an operator's name alone is not enough to make or remove
something that stops every other player. The same zones as the control socket's `zone` verbs;
changes are saved to `eden_zones.txt` and enforced on the next edit.

| Command | What it does |
|---|---|
| `/zone create <name> [exact]` | Protect the `//pos1`..`//pos2` box. By default the zone covers the selection's x/z **for the whole height of the world** (bedrock to sky), because a selection is usually drawn on the surface and a zone that stops at the surface can be tunnelled under. `exact` keeps the selection's own heights. No size cap. The zone has no bypass level; set one with `edenctl zone flags`. |
| `/zone rm <name>` | Remove a zone. |
| `/zone list [page]` | The zones, five a page. |
| `/zone here` | Which zones contain your feet (`off` zones included, marked). |

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
- **Protected zones are skipped, not refused.** Cells inside a zone
  ([configuration.md § Protected zones](configuration.md#protected-zones)) are left alone and the
  rest of the command applies; the reply says how many were skipped. Only a player logged in with
  a PIN, at or above a zone's bypass level, is not held by it.
- **Every command that changes cells is audited**, at every level, whether or not `--verbose` is
  on — see [configuration.md § The audit channel](configuration.md#the-audit-channel).
- **A large edit does not hold the world lock while it talks.** The batch is decided and written
  under the lock; the `ACTION` relay is formatted and sent after it is released. Building the
  relay is the larger half of the work, so this is most of the stall (measured: ~51 ms → ~13 ms
  for a full-cap 131,072-cell edit).

### Reply strings

The replies below are a **stable contract**: tools that drive the player commands over a game
connection (a bulk editor, a test harness) can match on them, and a change to any of them is a
breaking change that updates this table in the same commit.

Every reply is one line on the issuing connection only, in the form `[Server] <text>\n`. The
text is below; `<…>` is a substituted value, and `<verb>` is the command **as typed**
(`//strip` answers as `//strip`, not `//unpaint`).

**Any command**

| Reply | Meaning |
|---|---|
| `Unknown command '<verb>'. Try /help.` | Not a command. |
| `You do not have permission for that (level <n> required).` | Below the command's level. Nothing ran. |
| `Usage: <usage>` | Wrong argument count or shape. Any reply starting `Usage: ` means nothing ran. |
| `Commands are disabled on this server.` | The server runs with `--no-worldedit`. (`/login` is not a WorldEdit command and still works.) |
| *(no reply)* | The per-connection command pace was exceeded (a burst of 20, refilling at 5 per second). The line is dropped silently. A tool must pace itself or time out waiting for the reply. |

**Selection** — `//pos1`, `//pos2`

| Reply | Meaning |
|---|---|
| `pos1 = <x>, <y>, <z>` | Corner set; the other corner is not set yet. `pos2` for `//pos2`. |
| `pos1 = <x>, <y>, <z>  (<n> cells)` | Corner set; the box between the corners holds `<n>` cells (two spaces before the `(`). |
| `pos1 = <x>, <y>, <z>  (<n> cells — over the <max> limit)` | Corner set, but the box is too big for any command to use. |
| `That corner is outside the world.` | Explicit corner refused; the selection is unchanged. |
| `The server does not have your position yet.` | A feet or `~` corner before the server has heard where you are. |

Until the client has sent its first position the server treats the player as standing at
`0, 0, 0`, so a feet or `~` corner that early is usually refused as outside the world, not
with the position message. A corner given entirely in numbers is never affected by either.

**Box edits** — `//set`, `//walls`, `//paint`, `//unpaint`, `//strip`, `//replace`,
`//replacenear`, `//sphere`, `//hsphere`, `//cyl`, `//hcyl`

| Reply | Meaning |
|---|---|
| `<verb>: <n> block(s) changed.` | **The completion line** — always the last reply of an edit that ran. `<n>` counts cells whose visible state changed; cells already as requested are not counted. `0` is a normal answer (for example `//set 0` over open sky). |
| `Set //pos1 and //pos2 first.` | A selection command without both corners. Nothing ran. |
| `That area is <n> cells; the limit is <max>.` | The selection, or the box a radius implies, is over `--we-max-cells`. Nothing ran. |
| `Slow down — you have spent your edit budget for now.` | The per-player cells-per-second budget is spent. Nothing ran and nothing was charged; retry later. |
| `The world is at its edited-cell limit; that edit was refused.` | The edit would add more new cells than the world has room for. Nothing was written. **Followed by** the completion line with `<n>` = `0`. |
| `<n> cell(s) skipped: protected area '<zone>'.` | `<n>` cells the edit would have changed lie in a [protected zone](protocol.md#protected-zones) (`<zone>` is the first one met) and were left alone; everything else applied. **Followed by** the completion line, whose `<n>` does not include them. Also sent by `//paste`, `//undo`, `//redo` and `//up`. |
| `Unknown block '<arg>'.` / `Unknown colour '<arg>'.` | A block or colour argument did not resolve. Nothing ran. |
| `Radius must be 0..512.` / `Height must be 1..256.` | Shape arguments out of range. Nothing ran. |
| `The server does not have your position yet.` | `//replacenear` and the shapes are centred on your feet. |

**History** — `//undo`, `//redo`

| Reply | Meaning |
|---|---|
| `Undid <n> block(s).` / `Redid <n> block(s).` | Completion line. |
| `Nothing to undo.` / `Nothing to redo.` | The stack is empty. |
| `Slow down — you have spent your edit budget for now.` | Refused before anything moved: the edit stays on its stack, and the same `//undo` works once the budget refills. |
| `The world is at its edited-cell limit; that edit was refused.` | As above, followed by `Undid 0 block(s).` / `Redid 0 block(s).` |

**Login** — `/login` (stage 8.6)

| Reply | Meaning |
|---|---|
| `Logged in as <name>. Your level is <n>.` | Success; the session now has the name's level and zone bypass. |
| `Wrong PIN.` | The PIN did not match (counted against the address). |
| `Wrong PIN. A PIN is 8 digits.` | Not eight digits; refused without counting. |
| `You are already logged in.` | Nothing to do. |
| `The name <name> has no PIN, so there is nothing to log in to.` | The name is claimed by name alone. |
| `Too many login attempts; wait a few seconds.` | This connection's attempt pace is spent. Nothing was checked. |
| `Too many wrong PINs from your address; try again later.` | The address is locked out. Nothing was checked. |
| `Your PIN was changed a moment ago; ask the operator for the new one.` | An operator re-issued the PIN while this one was being checked. |
| `Usage: /login <pin>` | Wrong argument count. |

Unsolicited, on the same connection: `The name <name> is protected. Type /login <pin> to use its
permissions.` right after the welcome message; `Your PIN was changed by the operator; /login again
with the new one.` / `Your PIN was removed by the operator.` when `passwd` / `unpasswd` logs the
session out.

**Zones** — `/zone` (stage 8.7)

| Reply | Meaning |
|---|---|
| `Zone commands need you to be logged in: /login <pin>.` | Level 2, but not logged in. Nothing ran. |
| `Protected zone '<name>' (<x0>,<y0>,<z0>)..(<x1>,<y1>,<z1>) created. Nobody can edit inside it in game.` | Created, saved, enforced. |
| `Cannot make that zone: <reason>.` | `invalid name`, `duplicate name`, `zone cap reached`, or a coordinate out of range. Nothing changed. |
| `Removed zone '<name>'.` / `There is no zone named '<name>'.` | `rm`. |
| `--- zones <page>/<pages> (<count>) ---` then `<name> (<x0>,<y0>,<z0>)..(<x1>,<y1>,<z1>)[ off][ level <n>]` | `list`. `No protected zones.` when there are none. |
| `You are in: <name>[ (off)], ….` / `You are not in a protected zone.` | `here`. |
| `Could not save the zones: <error>` | The file write failed; the live set is unchanged. |

**Ordering.** The changed cells reach **every** connected client, the issuer included, as
`ACTION:server:0:…` lines (the relay shape is in
[protocol.md](protocol.md#legacy-world-snapshot)). Replies and relay lines are queued
separately, and replies go first, so **the completion line can arrive before the relay lines
it describes**. A tool that needs the edit applied locally should count relay lines, or
re-request the region, rather than treat the completion line as "the world now matches".
