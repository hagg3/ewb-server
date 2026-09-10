# Running edenserver as a service (Linux / systemd)

This is the ops runbook for keeping a `edenserver` world running persistently on a
Linux host: a dedicated user, a systemd unit, a firewall rule, and backups.

Artifacts in this directory:

| File | Purpose |
|---|---|
| `edenserver.service` | systemd unit template (reads `/etc/edenserver.conf`) |
| `edenserver.conf.example` | the `EnvironmentFile` template — `EDEN_*` settings |
| `edenserver-writeconf` | validating, atomic writer for `/etc/edenserver.conf` (used by `edenadmin`) |
| `edenserverctl` | `start`/`stop`/`restart`/`status`/`logs`/`logs-tail`/`backup` wrapper |
| `edenserver-backup.{service,timer}` | scheduled `edenserverctl backup` (hourly) |
| `sudoers.d/edenadmin` | optional NOPASSWD drop-in so the `edenadmin` GUI drives this box over ssh |
| `fail2ban/` | optional filter + jail for OS-level banning of password guessers |
| `INSTALL.md` | this document |

`edenctl` (repo root) is the client for the operator control socket — `kick`, `ban`,
`save`, `setblock`, `fill`, `stop`, and more (`docs/commands.md`). Install it too.

Assumed layout (all paths configurable — see below):

```
/usr/local/bin/edenserver          the binary (built on this box)
/usr/local/bin/edenserverctl       the management wrapper
/usr/local/bin/edenctl             the control-socket client
/usr/local/bin/edenserver-writeconf the validating conf writer (for edenadmin)
/etc/edenserver.conf                EDEN_* settings, read by the unit
/var/lib/edenserver/                owned by the edenserver user
  world/                            the classic single world (EDEN_WORLD_DIR)
  worlds/<name>/                    a multi-world host points EDEN_WORLD_DIR here
  backups/                          timestamped backup dirs
```

---

## 1. Build on the box

**Do not copy a prebuilt `edenserver` from your Mac or from another VPS.**
`build_server.sh` produces a binary native to the machine's CPU — an arm64 build
(Apple Silicon, ARM VPS) will not run on an x86-64 VPS, and vice versa. Build
where you deploy.

```sh
# Debian / Ubuntu
sudo apt install build-essential zlib1g-dev
git clone https://github.com/hagg3/ewb-server && cd ewb-server
./build_server.sh                       # picks clang++ if present, else g++
sudo install -m 0755 edenserver /usr/local/bin/edenserver
sudo install -m 0755 ops/edenserverctl /usr/local/bin/edenserverctl
sudo install -m 0755 ops/edenserver-writeconf /usr/local/bin/edenserver-writeconf
sudo install -m 0755 edenctl /usr/local/bin/edenctl
```

`build_server.sh` runs the offline test suites as part of the build; a
green run is your smoke test that zlib linked and the binary works.

---

## 2. Create the service user and world directory

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin edenserver
sudo mkdir -p /var/lib/edenserver/world /var/lib/edenserver/backups
sudo chown -R edenserver:edenserver /var/lib/edenserver
```

Put your starting world in place (optional — the server creates an empty one
otherwise):

```sh
sudo -u edenserver cp /path/to/eden_world.model /var/lib/edenserver/world/
sudo -u edenserver cp /path/to/eden_signs.txt   /var/lib/edenserver/world/   # if you have signs
```

---

## 3. Install the unit and its config file

The unit no longer carries settings on its `ExecStart` line — they live in
`/etc/edenserver.conf`, which it reads via `EnvironmentFile=`. Install both:

```sh
sudo cp ops/edenserver.service /etc/systemd/system/edenserver.service
sudo install -m 0600 -o root -g root ops/edenserver.conf.example /etc/edenserver.conf
sudoedit /etc/edenserver.conf          # set EDEN_PORT, EDEN_NAME, EDEN_WORLD_DIR, EDEN_PASSWORD
sudo systemctl daemon-reload
```

`/etc/edenserver.conf` keys and the `${VAR}` vs `$VAR` expansion rule are documented in
[`docs/configuration.md`](../docs/configuration.md#the-systemd-environmentfile-etcedenserverconf)
and in the template's own header. In short: the five required keys are always set and expand as
braced `${VAR}` (one argument each); every optional spaceless flag goes in `EDEN_EXTRA_ARGS`,
a bare `$VAR` tail that disappears when empty.

Notes on the unit (full rationale is in the file's comments):

- `WorkingDirectory=/var/lib/edenserver` — only a fallback now; every world file is addressed
  absolutely off `${EDEN_WORLD_DIR}`, including `--control-socket`.
- `EnvironmentFile=-/etc/edenserver.conf` — the leading `-` tolerates a missing file so
  `daemon-reload` never breaks, but the server then errors on the empty flags. Create the file.
- `Restart=on-failure` — **not** `Restart=always`. `--idle-timeout` makes the
  server `exit(0)` cleanly when empty; `Restart=always` would treat that clean
  exit as something to restart and spin a start/idle/exit loop. The template
  does not pass `--idle-timeout`; if you add it (in `EDEN_EXTRA_ARGS`), keep `on-failure`.
- `TimeoutStopSec=20` — headroom for a graceful stop. The server has no `SIGTERM` handler yet,
  so `systemctl stop` still loses up to one autosave interval; `edenadmin` stops it through
  `edenctl stop` instead.
- `StandardOutput=journal` + the binary's own `std::unitbuf` give live logs.
  Run the binary directly (as `ExecStart` does) — don't wrap it in a shell
  pipeline or `stdbuf`, which would re-buffer its output.

---

## 4. Firewall

Open **only** the game port (whatever you set `--port` to):

```sh
# ufw
sudo ufw allow 27015/tcp comment 'edenserver game port'

# firewalld
sudo firewall-cmd --permanent --add-port=27015/tcp
sudo firewall-cmd --reload
```

- Open `27020/tcp` **only** if you are self-hosting a matchmaker on this box
  (Phase 2C). A plain IP-joinable server does not need it.
- **Never** expose the operator control socket. It is a `0600` unix-domain
  socket inside the world directory (`edenserver.sock`) — it has no TCP port,
  there is nothing to open, and nothing should be forwarded to it. Being able
  to open the file is the authorisation; keep it owned by the `edenserver` user.
  Drive it with `edenctl` (see `docs/commands.md`).

---

## 5. Enable and start

```sh
sudo systemctl enable --now edenserver
edenserverctl status
edenserverctl logs            # journalctl -u edenserver, follows
```

`enable` wires it to `multi-user.target` so it comes back after a reboot;
`--now` also starts it immediately.

---

## 6. Backups

```sh
edenserverctl backup
# -> /var/lib/edenserver/backups/20260908T181104Z/{eden_world.model,eden_players.txt,eden_signs.txt}.gz
```

Backups are **gzipped**. `eden_world.model` is one plaintext `x:y:z:type:color` line per edited
cell, which deflates hard — a 40 MB world lands at about 7 MB — and nothing reads a backup
directly, so an hourly timer keeping raw copies just burns disk. `EDEN_BACKUP_LEVEL` (default 6)
tunes it; 9 buys a few percent for several times the CPU. `EDEN_BACKUP_COMPRESS=0` stores plain
copies as before. Restore a file by decompressing it back into the world dir:

```sh
edenserverctl stop
gunzip -c /var/lib/edenserver/backups/20260908T181104Z/eden_world.model.gz \
    > /var/lib/edenserver/world/eden_world.model
edenserverctl start
```

Run it on a schedule. The shipped systemd timer (hourly, with catch-up after downtime) is the
recommended path — it sources `/etc/edenserver.conf`, so it always backs up whichever world
`EDEN_WORLD_DIR` names:

```sh
sudo cp ops/edenserver-backup.service ops/edenserver-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now edenserver-backup.timer
systemctl list-timers edenserver-backup.timer
```

Or a plain cron line if you prefer:

```sh
# /etc/cron.d/edenserver-backup — hourly
0 * * * * edenserver /usr/local/bin/edenserverctl backup >/dev/null
```

Neither prunes old backups — add a second line / timer for that (see the VPS guide's Part 6).

`edenserverctl backup` asks the running server to `save` first (via `edenctl` on
the control socket) so the copy is current, then compresses each world file into
the snapshot dir via a staged `.tmp` + `mv`, so a crash mid-backup can't leave a
truncated `.gz` that looks complete. If the server is stopped or the control
socket is unavailable it skips the `save` and snapshots what is on disk, which is
still safe: the server publishes each world/player file with a temp-write +
`rename(2)`, so a backup never catches a torn file.

---

## 7. Verifying the three exit criteria

**a. Survives a reboot.**

```sh
sudo systemctl reboot
# after it comes back:
edenserverctl status            # Active: active (running)
systemctl is-enabled edenserver # enabled
```

**b. Backup works.**

```sh
edenserverctl backup
ls -l /var/lib/edenserver/backups/*/    # world/player(/sign) .gz files, non-zero size, current timestamp
gunzip -t /var/lib/edenserver/backups/*/*.gz && echo "archives intact"
```

Restore test: stop the service, `gunzip -c` a backup's files back into `world/`,
start, and confirm the world loads (`Loaded N world cells` in the log).

**c. journald has live logs.**

```sh
edenserverctl logs
# in another shell, join the server (or send it a PING); the log line should
# appear immediately, not after a delay — that is the std::unitbuf + journal path
# working end to end.
journalctl -u edenserver --since "10 min ago"   # history is retained
```

**d. the audit trail is there.**

Every change to the world or to a player's standing — from the control socket or from an in-chat
command — is logged unconditionally as an `[Audit]` line with a UTC timestamp:

```sh
journalctl -u edenserver | grep '\[Audit\]'
edenctl say "audit check"        # should produce one immediately
```

If you want a copy that outlives journald's retention, add
`--audit-file /var/lib/edenserver/world/audit.log` to `EDEN_EXTRA_ARGS` in
`/etc/edenserver.conf` (the path has no spaces, so the bare-`$` tail is fine) and rotate it
like any other log — the server reopens the file per line, so `logrotate` needs no
`copytruncate` and no signal.

---

## 8. Optional: the `edenadmin` operator GUI

`edenadmin` is a small Go binary you run **on your own machine** (not the VPS). It serves a
local web UI that drives this server over `ssh` — the same `ssh` key you already use, no new
port and no new credential on the box. Build and run it from `admin/` (see `admin/README.md`).

For the GUI to work without hitting an interactive `sudo` password prompt, the VPS needs:

- **A NOPASSWD sudoers drop-in.** Install `ops/sudoers.d/edenadmin` and edit the two aliases at
  the top (your ssh login user, and the service user):

  ```sh
  sudo install -m 0440 -o root -g root ops/sudoers.d/edenadmin /etc/sudoers.d/edenadmin
  sudoedit /etc/sudoers.d/edenadmin        # set EDENADMIN = <your-login-user>
  sudo visudo -cf /etc/sudoers.d/edenadmin
  ```

  If your ssh login user *already* has unrestricted `NOPASSWD` sudo, you only need
  `Defaults:<user> !requiretty` and can skip the command allowlist — but keeping the drop-in is
  harmless belt-and-braces. Possession of the ssh key is the actual admin boundary; this file
  just spares the GUI a blocking prompt.

- **Journal read access.** Reading `journalctl -u edenserver` as a non-root user needs
  membership in `adm` or `systemd-journal`, and it fails *silently empty* without it:

  ```sh
  sudo usermod -aG adm <your-login-user>      # log out and back in for it to take effect
  ```

The Connection panel probes every one of these and tells you which is missing.

## 9. Optional: fail2ban for password guessers

Only relevant if you run with `--password`. `edenserver` already throttles a
single-IP brute force itself (`--auth-fail-limit`: 5 wrong `JOIN` passwords in
60 s, then an escalating `accept()`-level lockout from 1 min up to 1 h). fail2ban
is the second layer — it pushes a persistent guesser's ban down to the firewall
so it is dropped before the TCP handshake, and it survives a server restart.

```sh
sudo cp ops/fail2ban/edenserver-auth.conf  /etc/fail2ban/filter.d/edenserver-auth.conf
sudo cp ops/fail2ban/jail-edenserver.local /etc/fail2ban/jail.d/edenserver.local
sudoedit /etc/fail2ban/jail.d/edenserver.local     # set port = to your --port
sudo systemctl reload fail2ban
sudo fail2ban-client status edenserver
```

Neither layer stops a *distributed* guesser (many IPs, few tries each). If a
password is your only gate against strangers, keep it long, or move to the ban
list / a future allow-list instead of a shared secret.
