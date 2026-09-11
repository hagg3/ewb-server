# Importing a `.eden` world

`eden_import` converts a saved Eden world (`.eden`, as the game and the community editors write
it) into a world this server can host — terrain, caves, signs and a spawn point.

It is an **offline tool**. Run it before the server starts; it never talks to a running server
and never touches the network.

```bash
./build_server.sh                          # builds ./eden_import along with everything else
./eden_import ~/Downloads/myworld.eden     # writes worlds/myworld/
./host_world.sh worlds/myworld/eden_world.model "My World" 27015
```

> ⚠️ **`worlds/` is tracked in this repository.** An imported personal world would be committed
> by a `git add -A`. `eden_import` writes a `.gitignore` into every directory it creates to stop
> that — do not delete it, and do not `git add -f` a world you did not author and do not intend
> to publish.

---

## What it reads

| Input | Where it comes from |
|---|---|
| `world.eden` | the world file itself, raw or inside a ZIP (detected by magic, not by extension) |
| `signs_world.eden.dat` | the sign sidecar, if one sits next to the input. Override with `--signs`, disable with `--no-signs` |
| the world's inline sign trailer | used only when there is no sidecar — the same precedence the game uses |

The format is parsed by `eden_file.h`, reimplemented clean-room from the public references
(Robert Munafo's format notes and the C# `EdenWorldManipulator`).

Briefly: a 192-byte header (seed, player position, "home", yaw, directory offset, name, version,
16 sky colours), then dense `(type, paint)` voxel grids one per saved 16×16 chunk, then an
optional reserved block for creature data, then a directory of `{i32 x, i32 y, u64 offset}` rows,
optionally followed by an appended sign section. Chunks are either 32,768 bytes (a 64-high world)
or 131,072 (256-high); the header's `version` field is a hint and is *not* reliable, so the
height format is detected from the file's own layout.

## What it writes

```
worlds/<name>/
  eden_world.model   x:y:z:type:color, one cell per line   (docs/configuration.md)
  eden_signs.txt     x:y:z:a:b:c:text, one sign per line
  eden_spawn.txt     x:y:z — the server sends a spawnless joiner here
  .gitignore         keeps the above out of commits
```

Everything is written with a temp file and a `rename()`, the same discipline the server's own
saves use: a crash or a full disk mid-write cannot leave a truncated world.

---

## Coordinates

`.eden` puts the horizontal plane in `(x, y)` and height in `z`. This server puts the plane in
`(x, z)` and height in `y`. That single rename is the whole conversion:

```
server(x, y, z) = eden(x, z, y)
```

Both formats centre the plane on block 65536 and both store height as an absolute value from 0,
so there is no origin offset on any axis. Signs get the identical rename and nothing more; their
`a`, `b`, `c` fields have no known meaning and are passed through verbatim, exactly as
[configuration.md](configuration.md) requires of `eden_signs.txt`.

Block ids and paint indices are **never translated**. The server does not interpret them either —
it re-streams the bytes and the client renders them.

---

## The base terrain profile

The client draws a fixed flat terrain for ground it has never been told about, and a `.eden`
stores that same profile verbatim for every untouched column:

| height | block | id |
|---|---|---|
| `y = 0` | bedrock | 1 |
| `y = 1..15` | stone | 2 |
| `y = 16..31` | dirt | 3 |
| `y = 32` | grass | 8 |
| `y >= 33` | air | 0 |

This is identical in the 64-high and 256-high formats. It is why the default strategy is cheap:
a world only has to record the voxels that *differ* from this, and untouched rock costs nothing.

> **Status of this table.** Directly observed. Joining the retail client on a server with an
> empty world and digging a column from the surface to bedrock gave grass at the surface, 16
> dirt below it, 15 stone below that, and one layer of bedrock at the bottom — grass `y = 32`,
> dirt `y = 16..31`, stone `y = 1..15`, bedrock `y = 0`, all unpainted (ROADMAP stage 5.4,
> 2026-09-09). This also matched every column of two authored `.eden` specimens in both height
> formats, the header's recorded standing height, and the real server streaming no ground plane
> at all. If it ever turns out wrong the symptom is visible banded terrain, and the fix is a
> `--base-profile` file, not a code change.

You can supply your own with `--base-profile FILE`:

```
# height[-height] : type [: paint]     — unmentioned heights are air
0:1
1-15:2
16-31:3
32:8
```

`--base-profile none` assumes nothing about the client.

---

## Strategies

`--air-fill` decides which voxels of a saved chunk become cells.

| Value | Emits | Use when |
|---|---|---|
| **`diff`** (default) | only voxels that differ from the base profile | almost always. Caves, overhangs and sunken ground all survive, at roughly 1 % of a full dump |
| `solid` | solid blocks only, no air | you are pasting a surface build onto default terrain and do not care about sub-surface voids — **they will fill in** |
| `full` | every voxel of every saved chunk | you want a byte-faithful import that assumes nothing about the client. Only viable on small worlds |

For scale, on a lightly-built 39-chunk world: `diff` ≈ 3,500 cells, `solid` ≈ 333,000, `full`
≈ 639,000.

`--base-profile` only affects `diff`; the other two do not consult a profile at all.

---

## The two budget numbers

`eden_import` prints two projections before it writes anything, and refuses rather than write a
world that fails either.

**Cells, against the cap.** Every line of `eden_world.model` becomes one entry in the server's
in-memory world map, and the server refuses new cells past its `--max-world-cells` (default
4,000,000 — see [configuration.md](configuration.md)). Importing a bigger world takes *both*
`eden_import --max-world-cells N` and a server started with a raised cap of its own.

⚠️ **The server's cap must be higher than the import, not equal to it.** Every block a player
places in open air, and every natural block they mine or paint, is a new cell. A server started
at exactly the imported cell count refuses all of them while still accepting edits to cells the
import already holds — so some builds save and others are gone on the next join. The summary's
`server cap` line is the value to use: the cell count plus a quarter, at least a million more,
rounded up to 100,000. An import that fits under `--max-world-cells` but would leave less room
than that gets a warning.

**Worst-case records in a single `REGION` reply.** This is the number that decides whether an
imported world actually *plays*, and it is not the same number. A `REGION` request is answered
with every cell inside a 29×29-chunk box, and clients re-request regions roughly every 750 ms.
`eden_import` slides that box over the world and reports the heaviest position.

It reports **records**, not cells, because that is what goes on the wire: air is one record, a
plain solid is one, and a *painted* solid is two (a type record and a colour record).

For reference, the largest reply ever captured from the real Eden server was about 507,000
records — roughly 3 MB on the wire and 395 ms to encode. `eden_import` warns above that and
refuses above 2,000,000 (`--max-region-records`, `0` disables it). A dense fill of one box would
be around 7 million records: 141 MB inflated and several seconds per request, on a path the
client hits twice a second. That is the failure this number exists to catch, and after the fact
it looks like a server bug rather than an import choice.

---

## Anomalies

| Condition | Behaviour |
|---|---|
| block type `255` | **hard error.** 255 is the server's internal painted-base sentinel, so such cells would be silently dropped from every `REGION` reply. Game block ids run 0–127, so this should never occur |
| block type `> 127` | warning; kept verbatim. The client may know blocks this tool does not |
| paint `> 54` | warning; kept in the model file. The server drops an out-of-palette colour on the wire, so the block renders unpainted |
| a sign outside the server's coordinate range | dropped and counted |

`--strict` promotes the two warnings to errors.

---

## Spawn

`--spawn` chooses what goes into `eden_spawn.txt`:

| Value | Meaning |
|---|---|
| `header` (default) | the world header's player position |
| `home` | the header's "home" point |
| `X,Y,Z` | explicit server coordinates |
| `none` | write no spawn file |

The header's position is already in server axis order and needs no rename. `home` is not the
default: it is an operator choice inside the game and its recorded height is not reliably a
place a player can stand.

The server reads `eden_spawn.txt` at startup (from the world's own directory, or `--spawn-file`;
`--spawn x:y:z` overrides it). A joining player who has no `eden_players.txt` row is sent there;
a returning player keeps their saved position.

---

## Interactive session

Run on a real terminal with nothing pinned on the command line, `eden_import` asks before it
writes. Every question has a flag that pre-answers and suppresses it, and a default equal to the
non-interactive behaviour — so a pipe, a redirect, or `--yes` runs straight through with no
prompts and nothing a script sees changes.

```
$ ./eden_import ~/Downloads/castle.eden

Strategy — what becomes a stored cell:
  name   cells         worst REGION   verdict
  diff   184,220       12,880         ok
  solid  1,910,540     221,700        ok
  full   9,930,112     1,344,600      over the cell cap
diff is faithful (caves and all) at ~1% of full; see docs/import.md.
strategy [diff]:

Spawn point written to eden_spawn.txt:
  header  65536.00, 33.92, 65540.00
  home    65500.00, 246.00, 65500.00  (out of range)
  none    do not write a spawn
spawn (header/home/none) [header]:

world directory name [castle]:
worlds/castle exists — overwrite? (y/N) [n]: y
```

| Prompt | Flag that skips it | Default |
|---|---|---|
| strategy | `--air-fill` | `diff` |
| spawn source | `--spawn` | `header` |
| output directory name | `--name` / `--out` | slug of the world's name |
| overwrite confirmation | `--force` or `--yes` | abort (`n`) |

---

## Flags

```
eden_import <world.eden> [options]

  --name NAME              world directory name (default: a slug of the world's own name)
  --out DIR                write here instead of worlds/<name>/
  --force                  overwrite an existing output directory
  --dry-run                project and print the summary, write nothing
  -y, --yes                accept every prompt's default; no interactive questions

  --air-fill diff|solid|full          what becomes a cell (default: diff)
  --base-profile default|none|FILE    the terrain `diff` compares against

  --signs FILE             sign sidecar (default: signs_<input>.dat beside the input)
  --no-signs               ignore signs entirely
  --spawn header|home|X,Y,Z|none      default: header

  --max-world-cells N      cell ceiling (default: 4000000, the server's own default)
  --max-region-records N   worst-case single-REGION reply ceiling (default: 2000000; 0 = off)
  --region-radius N        match a server started with --region-radius (default: 224)
  --strict                 treat unknown block ids and out-of-range paints as errors

  -h, --help
```

## What is not converted

Export (writing a `.eden` back out), live world switching, sky colours (the wire protocol has no
world-metadata or sky message — the summary prints them for information only), creature/entity
data, and any invented meaning for the sign `a`/`b`/`c` fields.
