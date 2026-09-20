# Exporting a world back to `.eden`

`eden_export` converts a world this server hosts back into a `.eden` file — the format the
offline Eden editors and the game itself read. It is the inverse of [`eden_import`](import.md),
and together the two close the loop: take a live world offline, edit it in an editor, publish it
again.

It is an **offline tool**. Run it while the server for that world is **stopped**, or run it
against a copy (an unpacked backup is the intended input). It never talks to a running server
and never touches the network.

```bash
./build_server.sh                                        # builds ./eden_export too
./eden_export worlds/myworld --out ~/myworld.eden        # writes the .eden + its sign sidecar
./eden_import ~/myworld.eden --out worlds/myworld2       # and straight back again
```

---

## What it reads

| Input | Required | Notes |
|---|---|---|
| `<world-dir>/eden_world.model` | yes | either format — `EDMB` binary or the legacy text form, detected by magic (`world_store.h`) |
| `<world-dir>/eden_signs.txt` | no | `x:y:z:a:b:c:text`, one sign per line |
| `<world-dir>/eden_spawn.txt` | no | `x:y:z`; becomes the header's player position |
| `<world-dir>/eden_origin.txt` | no | the source `.eden` header's seed, yaw, `home`, sky palette and height format, as `eden_import` recorded them — see [§ The origin sidecar](#the-origin-sidecar) |

`eden_players.txt` is **never read or written**. Player positions are not world data, and a world
handed to someone else has no business carrying them.

## What it writes

```
<file>.eden              the world
signs_<file>.eden.dat    the sign sidecar, beside it   (only when there are signs)
```

The sidecar name is not arbitrary: it is exactly what the game looks for, and exactly what
`eden_import`'s `--signs` default finds, so `eden_export … --out w.eden` followed by
`eden_import w.eden` closes the round trip with no extra argument. Use `--signs inline` to append
the signs inside the `.eden` itself instead (the appended `SGN1` trailer), or `--signs none` to
drop them.

Both files are written with a temp file and a `rename()`, the same discipline the server's own
saves use, and they are staged before either is published: a crash or a full disk mid-write
cannot leave a truncated world, nor a `.eden` without its signs.

**Nothing is written on a refusal.** Every check — arguments, the world file, the size ceiling —
happens before the first byte lands.

---

## The file it produces

```
[0, 192)          header: seed, pos, home, yaw, directory offset, name, version, sky colours
[192, dir)        chunk blocks — dense (type, paint) voxel grids, one per emitted chunk
[dir, …)          directory rows: {i32 cx, i32 cy, u64 offset}
[…, EOF)          with --signs inline: the SGN1 trailer, every row tagged cx = 0xffffffff
```

Written by the same little-endian helpers `eden_file.h` reads with, and deterministic: chunks
ascend by `(cx, cy)`, so two runs over one world produce byte-identical files.

Eight rules decide what goes in:

1. **Chunk selection.** A chunk block is emitted only if the store holds at least one cell in it.
   An untouched chunk is absent from the directory and the client synthesizes the base profile
   for it — that is what keeps the file small, and it is exactly what import assumes.
2. **Fill, then overlay.** Each emitted chunk starts as a dense block of pure base terrain; the
   store's cells are then written over it.
3. **The two cell sentinels.** A mined cell (the server's `CELL_MINED`, which a `WorldStore`
   surfaces as logical type 0) becomes air, with no paint. `CELL_PAINTED_BASE` (255) becomes the
   base profile's own block at that height, carrying the stored colour as its paint.
4. **The axis rename, backwards.** Server `(x, y = height, z)` → file `(x, y = plane, z = height)`,
   for blocks and for signs — the mirror of the rename [import](import.md#coordinates) applies.
5. **Height format.** 64z (32,768-byte chunks, ceiling 63) unless a cell *or a sign* sits above
   height 63, in which case 256z (131,072 bytes, ceiling 255). `--z 64` / `--z 256` force it.
   Anything above the chosen ceiling is **dropped and counted**, never wrapped.
6. **Coordinate gate.** A chunk coordinate must be in `0..32767`, the range a `.eden` directory
   can address. A cell outside it is dropped and counted rather than written into a directory the
   game would misread.
7. **The empty world.** A world with no cells still has to be a *file*, and a `.eden` with no
   addressable chunk is not one. An empty world therefore gets a single chunk of pure base
   terrain at the spawn's chunk coordinate; re-importing it yields zero cells, which is what it
   started as. The summary says `chunks: 1  (empty world: one base chunk)`.
8. **Optional ZIP wrapper** (`--zip`). The parser and the editors detect the container by magic,
   so both forms work. Plain is the default: it is the simpler artifact to diff and to debug.

> **The base profile must match.** Export fills untouched columns with the profile `eden_import`
> diffs against. If you imported a world with `--base-profile FILE`, export it with the same
> `--base-profile FILE` — otherwise the round trip is not a round trip, it is a re-terraform.

---

## The round-trip property

The correctness bar for this tool is a round trip, not a parse, and `eden_export_test.cpp`
states it as a property over every fixture (flat, carved, cave, 256z, mined, painted-base,
signed, empty, ZIP-wrapped):

```
import(export(import(f)))  has the same cell set as  import(f)
export(import(export(S)))  is byte-identical to      export(S)
```

The second form is the stronger one, and it is the one that covers the sentinels: a
painted-base cell re-imports as the explicit block it renders as, so the *cell* changes while
the *file* does not — and the world a player sees is the same either way.

Measured on a real 2.2-million-cell world: export → import → export is byte-identical, sidecar
included, and the cell count moves by 110 (see the absorption note below).

## What is lost

Nothing on the wire depends on any of this, but an editor round trip *looks* lossy, so here it
is in full:

| Lost | Why | Fix |
|---|---|---|
| seed, yaw, `home`, sky palette — **only for a world with no `eden_origin.txt`** | the server stores none of them; without the sidecar the header gets defaults (`home` mirrors the spawn) | a world made by `eden_import` has the sidecar and loses none of them; otherwise `--seed` / `--yaw`, or write the file by hand |
| the header's opaque bytes — the 32-character hash at 96–127, the 44-byte tail at 148–191, and whatever the game left after the name's NUL | their meaning is unknown, and the name area is stale memory in the specimens measured; inventing values is worse than writing zeros | none — every field the parser exposes is preserved; these are written as zero |
| creature / entity block | never parsed, never written | out of scope by design |
| a delta that equals the base terrain | once in the file it is indistinguishable from "untouched", so the next import drops it | none needed — it renders identically; only the cell count moves |
| a painted-base cell above the profile's last layer | there is no block there to recolour, so it is written as air and counted | place a real block instead |
| cells above the height ceiling, or outside the chunk-coordinate range | cannot be addressed by the format | `--z 256`; the summary names the counts |

## The origin sidecar

`eden_import` writes `eden_origin.txt` beside the world it makes; `eden_export` reads it if it is
there, so a world that came *from* a `.eden` goes back out as that same world rather than as
defaults. The server never reads it — it is inert state, like a photograph tucked into the
folder. The full grammar is in [configuration.md § `eden_origin.txt`](configuration.md#eden_origintxt).

What it replays: `seed`, `yaw`, `home`, the 16-band sky palette, the header `version`, and the
exact `pos`. A parsed header round-trips **bit-for-bit** — floats are stored with nine significant
digits, which reads back any `float` exactly, where `eden_spawn.txt`'s two decimals cannot.

Where two sources disagree, the more deliberate one wins:

- `--seed` / `--yaw` on the command line beat the file.
- An `eden_spawn.txt` the operator has **moved** beats the file's `pos`. One that still says what
  `eden_import` wrote (equal at its own two decimals) is the same spawn, so the sidecar's exact
  `pos` is used — the summary's `spawn:` line says `exact pos from eden_origin.txt`. With
  `eden_import --spawn none` there is no spawn file, and the sidecar's `pos` supplies the spawn.
  (`--spawn home` puts the header's `home` in `eden_spawn.txt`, so the round trip comes back with
  that as `pos` — you asked for it.)
- The height format is still decided by what is in the world. The sidecar's `z` is only a
  tie-breaker: a source that *was* 256z stays 256z once everything above height 63 has been
  edited away. It never lowers the ceiling, and `--z 64` overrides it.
- A recorded `version` is written only while it agrees with the format actually written (`>= 5`
  means 256z). A world that grew past 63 gets version 5, not a stale 4.

`--origin FILE` reads a sidecar from somewhere else, `--origin none` ignores it and writes
defaults. A named file that is missing is a refusal; a malformed line is skipped with a warning
naming it — never applied half-way.

---

## Exit codes, stdout and stderr

| | |
|---|---|
| **stdout** | the summary — one `key: value` per line — and the `wrote …` lines |
| **stderr** | warnings (dropped cells, dropped signs, unparseable input lines) and errors |
| **exit 0** | the export succeeded (or `--dry-run` finished) |
| **exit 1** | a refusal or an I/O failure. **No output file is written** |
| **exit 2** | a usage error: a missing or unknown argument |

The summary is written to be greppable by scripts:

```
world: myworld
source: worlds/myworld (EDMB, 2,200,053 cells)
z-format: 64z
chunks: 508
cells: 2200053
cells-dropped-height: 0
cells-dropped-coord: 0
signs: 2
signs-dropped: 0
signs-mode: sidecar
signs-file: /tmp/signs_myworld.eden.dat
spawn: 65548.37:47.92:65607.27 (eden_spawn.txt, exact pos from eden_origin.txt)
origin: worlds/myworld/eden_origin.txt
bytes: 16654464
file: /tmp/myworld.eden
```

`cells:` is the cell count that actually landed in the file — compare it against the source cell
count printed on the `source:` line, and against whatever count you derived from the world's own
`EDMB` header. They agree unless something was dropped, and a drop is always named on its own
`…-dropped-…` line *and* on stderr.

## Options

```
  eden_export <world-dir> --out <file.eden> [options]

  --out FILE               the .eden to write (required)
  --name NAME              world name in the header (default: the directory's name)
  --force                  overwrite an existing output file
  --dry-run                project and print the summary, write nothing
  --zip                    wrap the file in a ZIP container

  --base-profile default|none|FILE    terrain written into untouched columns
  --z auto|64|256          height format (default: auto)

  --signs sidecar|inline|none         default: sidecar
  --sidecar FILE           write the sidecar here instead

  --origin FILE|none       replay header fields from an eden_origin.txt
                           (default: <world-dir>/eden_origin.txt if present)
  --seed N                 world seed (default: 0, or the origin file's)
  --yaw F                  spawn facing (default: 0, or the origin file's)

  --max-bytes N            refuse to write a file larger than this
                           (default: 536870912, the parser's own inflate cap)

  -h, --help
```

## What it is not

Not a `.eden` → `.eden` editor, not an entity/creature writer, and not an attempt to reconstruct
terrain the server never stored. Export is the inverse of import, not more.
