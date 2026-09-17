#!/usr/bin/env bash
# ops/motd-edit.sh — interactive editor for a hosted world's welcome message.
#
# Run locally (on your own machine — it only needs `ssh`):
#     ./ops/motd-edit.sh
#
# Lists every world hosted on the server (the single-world `world` plus any
# edenserver@<instance> worlds, autodetected from the /var/lib/edenserver layout
# ops/INSTALL.md installs), lets you pick one, then show / set / clear its
# `eden_motd.txt` — the lines every joining player is shown, right after the
# built-in welcome line (docs/configuration.md § eden_motd.txt).
#
# Every change is applied the same way, and it never restarts anything:
#     1. write the new eden_motd.txt into the world directory (atomic rename),
#     2. `edenctl motd reload` on that world's control socket.
# The world, the players and the signs are never touched, and nobody is
# disconnected; the next player to join sees the new message.
#
# Why over ssh: the control socket is a 0600 unix socket inside the world
# directory on the server (it has no network address, by design — see
# ops/INSTALL.md § Firewall), so the only way to reach it from here is to run
# `edenctl` there.
#
# Config (env, both optional):
#   EDEN_VPS_HOST    ssh destination (default: eden-vps — set up a Host alias in
#                    ~/.ssh/config, the same one ops/backup-pull.sh uses)
#   EDITOR           editor for the multi-line edit (default: nano)
#
# Requires on the server, from ops/INSTALL.md: /usr/local/bin/edenctl, and sudo
# rights for your login user to write into /var/lib/edenserver and to run
# `edenctl` as the service user. A read-only `show` needs neither.

set -eu

HOST="${EDEN_VPS_HOST:-eden-vps}"
STATE_ROOT=/var/lib/edenserver
SVC_USER=edenserver
EDITOR_CMD="${EDITOR:-nano}"

echo "Server: $HOST   (state root $STATE_ROOT)"
echo

# --- Phase 1: discover worlds ------------------------------------------------
# Same layout rule as ops/backup-pull.sh: `world/` is the single-world unit
# (edenserver), every directory under `worlds/` is a template instance
# (edenserver@<name>). Output is one TAB-separated row per world:
#     <name> <dir> <unit> <motd line count or '-'> <ActiveState>
world_list=$(ssh -n "$HOST" "
    set -eu
    for d in '$STATE_ROOT'/world '$STATE_ROOT'/worlds/*/; do
        d=\${d%/}
        [ -d \"\$d\" ] || continue
        name=\$(basename \"\$d\")
        case \"\$d\" in
            '$STATE_ROOT'/world) unit=edenserver ;;
            *)                   unit=edenserver@\$name ;;
        esac
        if [ -f \"\$d/eden_motd.txt\" ]; then
            # awk, not \`grep -c\`: grep exits 1 on a zero count, which under the
            # local \`set -e\` would look like a failure rather than an empty MOTD.
            n=\$(awk '!/^[[:space:]]*(#|\$)/ {c++} END {print c+0}' \"\$d/eden_motd.txt\" 2>/dev/null || echo '?')
        else
            n=-
        fi
        state=\$(systemctl is-active \"\$unit\" 2>/dev/null || true)
        printf '%s\t%s\t%s\t%s\t%s\n' \"\$name\" \"\$d\" \"\$unit\" \"\$n\" \"\${state:-unknown}\"
    done
")

if [ -z "$world_list" ]; then
    echo "No worlds found under $HOST:$STATE_ROOT — is this the right host?" >&2
    exit 1
fi

names=(); dirs=(); units=()
i=0
echo "Worlds:"
while IFS=$'\t' read -r name dir unit motd state; do
    [ -n "$name" ] || continue
    names[i]="$name"; dirs[i]="$dir"; units[i]="$unit"
    case "$motd" in
        -)   motd_note="no MOTD" ;;
        0)   motd_note="MOTD file present but empty" ;;
        '?') motd_note="MOTD unreadable from here" ;;   # quoted: a bare ? is a glob
        *)   motd_note="$motd MOTD line(s)" ;;
    esac
    printf '  %d) %-20s %-22s %-10s %s\n' "$((i + 1))" "$name" "$unit" "$state" "$motd_note"
    i=$((i + 1))
done <<EOF
$world_list
EOF

echo
read -r -p "Pick a world [1-${#names[@]}]: " choice
if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt "${#names[@]}" ]; then
    echo "Not a valid choice." >&2
    exit 1
fi
idx=$((choice - 1))
world="${names[$idx]}"
world_dir="${dirs[$idx]}"
unit="${units[$idx]}"
motd_path="$world_dir/eden_motd.txt"
sock="$world_dir/edenserver.sock"

echo
echo "World '$world'  —  $unit"
echo "  MOTD file:      $motd_path"
echo "  Control socket: $sock"

# --- helpers -----------------------------------------------------------------

# Run a command on the server that needs `sudo`. `-t` allocates a tty so an
# interactive [sudo] password prompt works; the `tr` strips the CRs a tty adds.
ssh_sudo() {
    ssh -t "$HOST" "$1" 2>&1 | tr -d '\r'
}

# Current file contents on stdout (empty if there is no file).
fetch_motd() {
    ssh -n "$HOST" "cat '$motd_path' 2>/dev/null || true"
}

# What the running server is actually serving right now.
show_live() {
    echo
    echo "Live MOTD (what a joining player is sent):"
    ssh_sudo "sudo -u '$SVC_USER' edenctl -S '$sock' motd show" | sed 's/^/  /'
}

reload_motd() {
    echo "Reloading ..."
    ssh_sudo "sudo -u '$SVC_USER' edenctl -S '$sock' motd reload" | sed 's/^/  /'
}

# Install $1 (a local file) as the world's eden_motd.txt, then reload. Staged
# through the login user's /tmp (no sudo needed to copy it up) and renamed into
# place under the service user, so a half-written file is never visible under
# the real name and the server can always read the result.
push_motd() {
    local src="$1" remote_tmp="/tmp/eden_motd.$$"
    echo
    echo "Writing $motd_path ..."
    ssh "$HOST" "cat > '$remote_tmp'" < "$src"
    ssh_sudo "
        set -eu
        sudo install -o '$SVC_USER' -g '$SVC_USER' -m 0644 '$remote_tmp' '$motd_path.tmp' &&
        sudo mv '$motd_path.tmp' '$motd_path' &&
        rm -f '$remote_tmp' && echo '  written.'
    "
    reload_motd
}

# --- Phase 2: what do you want to do -----------------------------------------

while true; do
    echo
    echo "  1) Show the current MOTD"
    echo "  2) Edit the MOTD in \$EDITOR ($EDITOR_CMD)   — add or change"
    echo "  3) Set a one-line MOTD"
    echo "  4) Delete the MOTD (players see no welcome message)"
    echo "  5) Reload from the file on the server without changing it"
    echo "  q) Quit"
    echo
    read -r -p "Choice: " action

    case "$action" in
        1)
            echo
            echo "File on the server ($motd_path):"
            body=$(fetch_motd)
            if [ -z "$body" ]; then
                echo "  (no file, or it is empty)"
            else
                printf '%s\n' "$body" | sed 's/^/  /'
            fi
            show_live
            ;;
        2)
            local_tmp=$(mktemp -t eden_motd)
            trap 'rm -f "$local_tmp"' EXIT
            fetch_motd > "$local_tmp"
            if [ ! -s "$local_tmp" ]; then
                cat > "$local_tmp" <<'TEMPLATE'
# eden_motd.txt — shown to every player who joins this world, one chat line per
# line below. Lines starting with '#' and blank lines are ignored. At most 8
# lines, 256 bytes each. Delete everything to have no welcome message.
TEMPLATE
            fi
            before=$(cat "$local_tmp")
            "$EDITOR_CMD" "$local_tmp"
            after=$(cat "$local_tmp")
            if [ "$before" = "$after" ]; then
                echo "No changes; nothing sent."
            else
                echo
                echo "New MOTD:"
                sed 's/^/  /' "$local_tmp"
                read -r -p "Apply to '$world'? [y/N] " ok
                case "$ok" in [yY]*) push_motd "$local_tmp" ;; *) echo "Cancelled." ;; esac
            fi
            rm -f "$local_tmp"; trap - EXIT
            ;;
        3)
            read -r -p "MOTD line: " oneline
            if [ -z "$oneline" ]; then
                echo "Empty — nothing sent. Use option 4 to remove the MOTD."
                continue
            fi
            local_tmp=$(mktemp -t eden_motd)
            trap 'rm -f "$local_tmp"' EXIT
            printf '%s\n' "$oneline" > "$local_tmp"
            read -r -p "Set '$world' MOTD to that single line? [y/N] " ok
            case "$ok" in [yY]*) push_motd "$local_tmp" ;; *) echo "Cancelled." ;; esac
            rm -f "$local_tmp"; trap - EXIT
            ;;
        4)
            read -r -p "Delete $motd_path on '$world'? [y/N] " ok
            case "$ok" in
                [yY]*)
                    ssh_sudo "sudo rm -f '$motd_path'"
                    reload_motd
                    ;;
                *) echo "Cancelled." ;;
            esac
            ;;
        5)
            reload_motd
            ;;
        q|Q|"")
            exit 0
            ;;
        *)
            echo "Not a choice."
            ;;
    esac
done
