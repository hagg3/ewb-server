# Running edenserver as a service (Linux / systemd)

This is the ops runbook for keeping a `edenserver` world running persistently on a
Linux host: a dedicated user, a systemd unit, a firewall rule, and backups.

Artifacts in this directory:

| File | Purpose |
|---|---|
| `edenserver.service` | systemd unit template |
| `edenserverctl` | `start`/`stop`/`restart`/`status`/`logs`/`backup` wrapper |
| `fail2ban/` | optional filter + jail for OS-level banning of password guessers |
| `INSTALL.md` | this document |

`edenctl` (repo root) is the client for the operator control socket — `kick`, `ban`,
`save`, `setblock`, `fill`, `stop`, and more (`docs/commands.md`). Install it too.

Assumed layout (all paths configurable — see below):

```
/usr/local/bin/edenserver          the binary (built on this box)
/usr/local/bin/edenserverctl       the management wrapper
/usr/local/bin/edenctl             the control-socket client
/var/lib/edenserver/                owned by the edenserver user
  world/                            WorkingDirectory — world/player/sign files live here
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

## 3. Install the unit

```sh
sudo cp ops/edenserver.service /etc/systemd/system/edenserver.service
sudoedit /etc/systemd/system/edenserver.service    # adjust ExecStart: --port, --name, paths
sudo systemctl daemon-reload
```

Notes on the unit (full rationale is in the file's comments):

- `WorkingDirectory=/var/lib/edenserver/world` — the server reads and writes
  `eden_world.model`, `eden_players.txt`, `eden_signs.txt` relative to its CWD.
- `Restart=on-failure` — **not** `Restart=always`. `--idle-timeout` makes the
  server `exit(0)` cleanly when empty; `Restart=always` would treat that clean
  exit as something to restart and spin a start/idle/exit loop. The template
  does not pass `--idle-timeout`; if you add it, keep `on-failure`.
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
# -> /var/lib/edenserver/backups/20260908T181104Z/{eden_world.model,eden_players.txt,eden_signs.txt}
```

Run it from cron / a systemd timer as often as you like:

```sh
# /etc/cron.d/edenserver-backup — hourly
0 * * * * edenserver /usr/local/bin/edenserverctl backup >/dev/null
```

`edenserverctl backup` asks the running server to `save` first (via `edenctl` on
the control socket) so the copy is current, then does an atomic-safe `cp` of each
world file. If the server is stopped or the control socket is unavailable it
falls back to a plain `cp`, which is still safe: the server publishes each
world/player file with a temp-write + `rename(2)`, so a backup never catches a
torn file.

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
ls -l /var/lib/edenserver/backups/*/    # world/player(/sign) files, non-zero size, current timestamp
```

Restore test: stop the service, copy a backup's files back into `world/`, start,
and confirm the world loads (`Loaded N world cells` in the log).

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

If you want a copy that outlives journald's retention, add `--audit-file
/var/lib/edenserver/world/audit.log` to the unit's `ExecStart` and rotate it like any other log
(the server reopens the file per line, so `logrotate` needs no `copytruncate` and no signal).

---

## 8. Optional: fail2ban for password guessers

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
