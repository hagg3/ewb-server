# edenadmin — local operator GUI

`edenadmin` is a small Go binary that runs on your own machine and serves a web
UI at `http://127.0.0.1:<port>`. It manages a standalone Eden server by running
the tools that already exist — `edenctl` over the control socket, `eden_import`
for `.eden` world conversion, and (for a remote server) `edenserverctl` /
`systemctl` / `journalctl` over `ssh`.

It adds **no listening port on the server and no new credential**. For a remote
server, admin access is possession of the SSH key — the same as today. The HTTP
server binds loopback only, is up only while `edenadmin` runs, and is guarded by
a per-run token plus an `Origin`/`Host` allowlist.

> **Status:** stage 6.7, in progress (see `WORKING/ROADMAP-SERVER.md`). Shipped:
> the HTTP shell, the loopback guard, profile storage, the **Connection** panel,
> the reply parsers + golden corpus, the plain-process supervisor, the read-only
> **Status / Players / Logs** panels, the **write actions** — Power
> (start/stop/restart/kill), the **Bans** panel, and say / kick / op / deop from
> **Players** — the **Worlds** panel: list, bundle download/upload,
> `.eden` import (project → write), and set-active (both transports) — the
> **Backups** panel: list, backup now, and a safety-copied stop→swap→start
> restore — the **Config** panel: a typed form over `/etc/edenserver.conf`
> (vps) or the profile args (local), with a diff and an optional restart — and
> the **Audit** panel: the `[Audit]` channel, from a dedicated `--audit-file`
> or (fallback) filtered out of the live log / journal. Still owed for 6.7: the
> live VPS pass (every panel against a real `vps` profile), blocked on a Linux
> host.

## Build

Needs a Go toolchain (`go` 1.23+):

```sh
brew install go            # macOS
# or: apt install golang   # Debian/Ubuntu; or https://go.dev/dl/

./admin/build.sh           # go vet + go test + build -> ./admin/edenadmin
```

Or from the repo root: `./build_server.sh --with-admin`. The plain
`./build_server.sh` does **not** build `admin/` — a server host that does not
want the GUI should not need Go.

## Run

```sh
./admin/edenadmin              # binds an ephemeral loopback port, opens a browser
./admin/edenadmin --port 8765  # a fixed port
./admin/edenadmin --open=false # print the URL, do not launch a browser
```

The launch URL carries the session token as a `#t=…` fragment. Open exactly that
URL; the fragment never reaches a server log. `Ctrl-C` stops the server and
closes the port.

Flags:

| Flag | Default | Meaning |
|---|---|---|
| `--port` | `0` (ephemeral) | loopback port to bind |
| `--profiles` | `~/.config/edenadmin/profiles.toml` | profile store path |
| `--open` | `true` | open the URL in the default browser |

## Profiles

`~/.config/edenadmin/profiles.toml`, mode `0600`. **No secrets** — SSH keys stay
in `~/.ssh`, the world password stays on the server. Edit it in the Connection
panel or by hand.

```toml
active = "dev"

[profiles.dev]
kind        = "local"
world_dir   = "/Users/you/ewb-server/worlds/dev"
world_root  = "/Users/you/ewb-server/worlds"
server_bin  = "/Users/you/ewb-server/edenserver"
eden_import = "/Users/you/ewb-server/eden_import"   # default: next to server_bin
socket      = ""                                     # default: <world_dir>/edenserver.sock
backup_dir  = ""                                     # default: <world_dir>/backups
args        = ["--port", "27015", "--name", "Dev World"]

[profiles.prod]
kind        = "vps"
ssh_host    = "eden-vps"                # a Host alias in ~/.ssh/config
service     = "edenserver"             # systemd unit
run_as      = "edenserver"             # the service user; edenctl calls run `sudo -n -u` this
world_dir   = "/var/lib/edenserver/world"
world_root  = "/var/lib/edenserver/worlds"
env_file    = "/etc/edenserver.conf"
eden_import = "/usr/local/bin/eden_import"
```

`local` is a plain `edenserver` process on this machine (no systemd). `vps`
drives a systemd-managed server over `ssh`, using connection multiplexing so the
polling panels do not open a new SSH connection per request.

### `local` transport

`edenadmin` never shells out for information on a `local` profile — it reads
files and stats paths directly. It only ever spawns `./edenserver` and runs
`./edenctl` / `./eden_import`.

A `local` server is run by `edenadmin`'s own process supervisor: it spawns
`edenserver` with `setsid` (so quitting `edenadmin` does not take the world
down), appends stdout+stderr to `<world_dir>/edenserver.log`, and tracks the pid
in `<world_dir>/edenadmin-supervisor.pid` together with the process's real start
time so a recycled pid is never mistaken for a live server. Graceful stop always
goes through `edenctl stop` (which saves the world) before any signal. The
Status panel therefore only shows a running server that `edenadmin` started.

### Write actions (stage 6.3)

| Panel | Action | Route | How it runs |
|---|---|---|---|
| Status | **Power**: start / stop / restart / kill | `POST /api/power` | `local`: the supervisor (`edenctl stop` → SIGTERM → SIGKILL for stop). `vps`: **stop/restart go through `edenctl stop`** — never `systemctl stop`, because the server installs no `SIGTERM` handler and systemd's stop would discard up to one autosave interval of edits (plan §5.4). Only **kill** is an ungraceful signal (`systemctl kill -s SIGKILL`); only start/kill touch `systemctl`. |
| Players | say / kick / op / deop, ban | `POST /api/players/action` | `edenctl say` / `kick` / `op` / `deop` / `ban` |
| Bans | list, add, remove | `GET`/`POST /api/bans` | `edenctl banlist` / `ban` / `unban` |

No optimistic UI: every action re-polls its panel and shows the server's own
reply line (classified by `parse.Ack` as ok / error / usage / unknown). Every
action has a confirm dialog. Free-text arguments (a `say` message, a kick
reason) are **rejected before dispatch if they contain a newline or carriage
return** — the control reader is `\n`-framed and `sanitize_text` runs after
framing, so a newline would make the tail of the string a second control verb.

### Worlds (stage 6.4)

Lists the hostable worlds under the profile's `world_root` (each a
`<name>/eden_world.model`), with the block count and which one is active.

| Action | Route | How it runs |
|---|---|---|
| List | `GET /api/worlds` | `local`: `os.ReadDir` + count `\n` in the model file. `vps`: `find … -name eden_world.model` + `wc -l`. |
| Download bundle | `GET /api/worlds/bundle?world=<name>` | a tar of `eden_world.model`, the sign / spawn / player sidecars, and the `.gitignore` — whichever exist. `local`: built in Go. `vps`: `sudo -n -u <run_as> tar -C <dir> --ignore-failed-read -cf -`. |
| Upload bundle | `POST /api/worlds/bundle?world=<name>` (multipart `bundle`) | **refused unless the server is stopped** — a live server rewrites `eden_world.model` on every autosave. The tar is validated (only known member names, must carry `eden_world.model`) before anything is written. |
| Import `.eden` | `POST /api/worlds/import?mode=project\|write` (multipart `eden` + form fields) | stages the upload to a temp path (a Go temp file locally, `dd of=…` as `<run_as>` on a vps), runs `eden_import --yes` (dry-run for `project`, real for `write`), parses the summary, then **removes the temp file on every path** — success, parse failure, or refusal. `mode=write` with `set_active=true` chains into set-active. An existing target dir needs `force=true` (→ `--force`). |
| Set active | `POST /api/worlds/activate` `{world, restart}` | `local`: rewrites `--world` (and `--signs`, dropped if the sidecar is absent) in the stored args, moves `world_dir`, persists, and optionally restarts the supervised process. `vps`: rewrites `EDEN_WORLD_DIR` in `/etc/edenserver.conf` through `edenserver-writeconf` (stage 6.6), raising `EDEN_MAX_WORLD_CELLS` to the imported world's recommended cap (its cell count plus room for players to build — see `docs/import.md`) when that is higher than the current value, moves the profile's `world_dir` pointer so socket / backup / restore follow, and restarts. |

The import form exposes the strategy (`--air-fill`), spawn source (`--spawn`),
`--strict`, `--no-signs`, and — for an acknowledged refusal — `--max-world-cells`
/ `--max-region-records`. **Project first**: the *Import* button only enables
after a clean (non-refused) projection. `docs/import.md` explains the two budget
numbers and why the server's `--max-world-cells` must sit above the imported
world's cell count, not at it.

### Backups (stage 6.5)

World-file backups under the profile's `backup_dir` (default:
`<world_dir>/backups` for `local`, `/var/lib/edenserver/backups` for `vps` —
the same default `edenserverctl` uses). A backup captures `eden_world.model`
and the sign / spawn / player sidecars, not the `.gitignore`.

Members are stored **gzipped**, as `<name>.gz` — a world file is plaintext and
deflates to roughly a sixth, and nothing reads a backup in place. Both producers
agree on that layout (`edenserverctl backup` for `vps`, `compressBackupMembers`
for `local`), and every consumer accepts a plain `<name>` too, so backups taken
before this change and hosts running `EDENSERVER_BACKUP_COMPRESS=0` stay
listable and restorable.

| Action | Route | How it runs |
|---|---|---|
| List | `GET /api/backups` | `local`: `os.ReadDir(backup_dir)` + per-dir size. `vps`: `du -k --max-depth=1 <backup_dir>`. A directory whose name is not a `YYYYMMDDTHHMMSSZ` stamp is still listed (flagged "odd name"); a `-prerestore` safety copy is flagged too. Newest first. |
| Backup now | `POST /api/backups/create` | `local`: `edenctl save` (if running) then gzip the world files into `backup_dir/<stamp>/` in Go. `vps`: `sudo -n -u <run_as> env EDENSERVER_WORLD_DIR=… EDENSERVER_BACKUP_DIR=… edenserverctl backup` (the `env` form must be in the sudoers rule). |
| Restore | `POST /api/backups/restore` `{stamp}` | **1.** back up the current world → `backup_dir/<stamp>-prerestore/` (a mandatory safety copy, taken before anything is touched). **2.** stop the server gracefully (`local`: the supervisor's `edenctl stop` → signals; `vps`: `edenctl stop`, never `systemctl stop`). **3.** confirm the process is gone — if it is still up, **abort before any file is swapped**. **4.** decompress the backup's members into the world dir; on a mid-swap failure, roll back from the safety copy. **5.** start the server again if it had been running. The safety copy is always kept. |

A `vps` restore inflates the members **locally**: `BackupPack` tars the stamp
dir over ssh, `plainBackupTar` rewrites that stream to plain names and bytes in
Go, and the result goes to the host's existing `tar -xf -`. That keeps the swap a
single unpack — no shell pipeline to quote and no new sudoers verb — and drops
any tar member that isn't a world file, so a hand-edited backup dir can't write a
path of its choosing into the world directory.

Restore is the most dangerous action in `edenadmin` — it has a two-step confirm
and the confirm text names the backup being restored. Backup *scheduling* is
`ops/edenserver-backup.timer` (`systemctl enable --now`, see `ops/INSTALL.md`);
a GUI toggle for it is a later follow-up.

### Config (stage 6.6)

A typed form over the server's configuration, with a line diff as the confirm gate and an
optional graceful restart after the write.

| Transport | Reads | Writes |
|---|---|---|
| `vps` | `cat /etc/edenserver.conf` (falls back to `sudo -n cat`) | streams the rebuilt file to `sudo -n /usr/local/bin/edenserver-writeconf` on stdin — the validating, atomic writer; **never `sudo tee`**. Then `edenctl stop` + `systemctl start` if "restart" is ticked. |
| `local` | the profile's `args` | rewrites `--port` / `--name` / `--max-world-cells` in the stored args and persists the profile. |

The form fields are `EDEN_PORT`, `EDEN_NAME`, `EDEN_WORLD_DIR`, `EDEN_MAX_WORLD_CELLS`,
`EDEN_PASSWORD` and `EDEN_EXTRA_ARGS`. A `vps` rewrite **preserves every comment, blank line and
unknown `EDEN_*` key** the operator added, replaces managed keys in place, and *omits*
`EDEN_EXTRA_ARGS` entirely when it is empty (it is a bare `$VAR` tail — see
`docs/configuration.md`). Validation rejects an out-of-range port, a non-absolute or
whitespaced world dir, `max_world_cells < 1`, and a newline in any value.

### Audit (stage 6.7)

The server's audit channel — one UTC-stamped line per state-changing control
command and per completed edit, with the actor (`control` or `player:<name>`).
The channel is unconditional on stdout, so there is always a source:

| Transport | Dedicated `--audit-file` set | No `--audit-file` (fallback) |
|---|---|---|
| `local` | tail that file (last 128 KiB) | filter `[Audit]` lines out of `<world_dir>/edenserver.log` |
| `vps` | `sudo -n -u <run_as> tail -n 2000 <path>` (the file belongs to the service user) | `journalctl -u <unit> -n 2000`, `[Audit]` lines only |

On a `vps` profile the `--audit-file` path is read out of `EDEN_EXTRA_ARGS` in
`/etc/edenserver.conf`. Each poll returns the whole current tail and the browser
replaces its view (audit volume is bounded by the edit path's cell budget). The
panel is polled every 3 s and the filter is client-side. The flag is off by
default, so a profile with no `--audit-file` and nothing logged yet shows an
explicit note recommending `--audit-file` for a restart-surviving feed — never a
blank panel. A journal that reads back empty is called out as the
`adm` / `systemd-journal` group gap, same as the Connection probe.

### Read-only panels (stage 6.2)

| Panel | Source | Notes |
|---|---|---|
| Status | supervisor pid + `syscall.Statfs` + `edenctl who` / `region-stats` | polled every 3 s; pauses while the browser tab is hidden |
| Players | `edenctl who` | flags an IP-shaped username (ambiguous with an IP ban) |
| Logs | `<world_dir>/edenserver.log`, tailed from a byte offset | resets on truncation/rotation; filter is client-side |

The reply parsers (`internal/parse`) are pinned by a golden corpus in
`admin/testdata/golden/`, captured from a real local `edenserver` / `eden_import`
(the host-format samples — `systemctl show`, `df -Pk`, `journalctl -o json` — are
hand-written Linux output pending the VPS pass in stage 6.7).

### `vps` transport — prerequisites

The control socket is `0600` and owned by the service user, so **every remote
`edenctl` call runs `sudo -n -u <run_as>`** and every `systemctl` verb runs
`sudo -n`. `-n` (non-interactive) everywhere means a missing sudoers rule fails
fast instead of hanging the GUI on a password prompt.

The Connection panel probes for exactly these gaps:

| Probe | Fails when |
|---|---|
| ssh reachable | the Host alias, the key, or the network is wrong |
| control socket (as `<run_as>`) | **the sudoers rule for `<run_as>` is missing** (surfaced explicitly), or the server is stopped (a soft note) |
| systemd unit | `sudo -n systemctl` is denied, or the unit is not installed |
| journal readable | the login user is not in `adm` / `systemd-journal` (the journal reads back **silently empty** — this probe catches that) |
| eden_import (as `<run_as>`) | the binary is absent or not runnable as the service user |

**Prerequisites on the VPS** (`ops/INSTALL.md` § 8 has the commands):

- `ops/sudoers.d/edenadmin` — a `NOPASSWD` drop-in for the specific commands the GUI runs as
  the service user and as root (`edenctl`, `eden_import`, `tar`/`dd`/`find`/`wc`/`du`,
  `systemctl {start,stop,restart,kill,show}`, `edenserver-writeconf`). Edit the two aliases at
  the top. An operator whose ssh login already has unrestricted `NOPASSWD` sudo only needs
  `Defaults:<user> !requiretty`.
- Journal-group membership (`adm` or `systemd-journal`) for the ssh login user — without it
  `journalctl -u edenserver` reads back **silently empty**, which the Connection panel's
  "journal readable" probe is there to catch.

## Tests

`./admin/build.sh` runs `go test ./...` — offline, no server, no network:

- `internal/transport` — `Shq` (with a round-trip through `/bin/sh`), the local
  and ssh dispatchers, timeout and missing-binary handling.
- `internal/eden` — every verb's argv for both transports, `eden_import`
  dry-run/real argv (with `--yes` always present), the world-bundle / temp
  verbs (`tar`, `dd`, `rm`) sudo-wrapped for `vps`, the backup verbs
  (`du`, `env … edenserverctl backup`, `tar` of a stamp dir), and the conf
  verbs (`cat`, `sudo -n cat`, `edenserver-writeconf`, the `env EDENSERVER_CONF=`
  form for a non-default `env_file`).
- `internal/profile` — the TOML round trip, validation rejects, `0600` on save.
- `internal/httpd` — the four guard rejections, the JSON envelope, the profile
  CRUD flow, a `local` connection test, the Worlds routes: list, a
  method-guarded `activate`, set-active rewriting `--world` in the persisted
  args, a bundle-download tar carrying `.gitignore`, and `import?mode=write`
  chaining into set-active (against a stub `eden_import`); the Backups
  routes: a method-guarded `create`/`restore`, create → list → restore; and the
  Config routes: the form shape on GET, an out-of-range port → 400, a valid POST
  rewriting the persisted local-profile args.
- `internal/panels` — the Connection probe logic for both transports (the `vps`
  path against a scripted fake transport); `parseWorldArgs`, the IP-shaped-name
  check, and the Status / Players / Logs panels against a scripted fake; the
  write actions — newline rejection before dispatch, error-reply classification,
  the server-down guard, and **`vps` stop/restart proven to route through
  `edenctl stop` and never `systemctl stop`/`kill`/`restart`**; a real
  supervisor start→kill via `/bin/sleep`; the Worlds panel — listing + active
  flag, the `--world`/`--signs` arg rewrite, a bundle round trip that preserves
  `.gitignore`, an upload refused while the server runs, and the `eden_import`
  temp file provably removed after both a success and a refusal; the Backups
  panel — listing sorted newest-first and tolerant of a non-stamp directory
  name, backup-now, restore taking the safety copy before the swap, restore
  rolling back to the pre-restore world on a mid-swap failure, and a `vps`
  restore aborting (no file swap, no `systemctl stop`) when the unit will not
  go inactive; the Config panel — `ConfFile` round trip preserving comments /
  key order / an operator's own key, `EDEN_EXTRA_ARGS` omitted when empty,
  settings validation rejects, the line diff, a `vps` write proven to reach
  `edenserver-writeconf` and restart via `edenctl stop` (never `systemctl
  stop`), a sudo-denied write surfaced legibly, `SetConfWorldDir` raising the
  cell cap, and the local args rewrite; the Audit panel — the `local` log
  fallback and a dedicated `--audit-file` (including "configured but not created
  yet"), the `vps` journal fallback, the `vps` dedicated file resolved from
  `EDEN_EXTRA_ARGS` and tailed as `<run_as>`, and a silent journal surfaced as
  the group gap.
- `internal/parse` — every reply parser round-tripped against the golden corpus,
  including the empty / truncated / CRLF / zero-header / over-cap / over-REGION /
  type-255 cases, `du -k --max-depth=1` (tab- and space-separated), the audit
  channel (bare lines and `journalctl -n` output with the syslog prefix), and a
  "no parser panics on junk" sweep.
- `internal/supervise` — the supervisor lifecycle against `/bin/sleep` (start →
  status → graceful stop → pidfile cleaned), double-start refusal, immediate
  exit, a stale pidfile whose pid now belongs to another process, and the log
  tailer across an append, a rotation and a size cap.
