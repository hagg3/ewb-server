#!/usr/bin/env bash
# ops/backup-pull.sh — interactive puller for edenserverctl backups.
#
# Run locally (on your Mac, from anywhere — it only needs `ssh`/`rsync`):
#     ./ops/backup-pull.sh
#
# Lists every world hosted on the VPS (the single-world "world" plus any
# edenserver@<instance> worlds, autodetected from the backup directory layout
# ops/edenserverctl writes — see docs/configuration.md § Ops wrapper), lets you
# pick one, lists its most recent backups, lets you pick one of those (or the
# latest), and rsyncs it down to a local directory. Nothing on the VPS is
# modified or deleted.
#
# Config (env, both optional):
#   EDEN_VPS_HOST           ssh destination (default: eden-vps — set up a Host
#                            alias in ~/.ssh/config; see WORKING/VPS-HOSTING-GUIDE.md)
#   EDEN_BACKUP_LOCAL_DIR   where pulled backups land (default: ~/eden-backups)

set -eu

HOST="${EDEN_VPS_HOST:-eden-vps}"
LOCAL_ROOT="${EDEN_BACKUP_LOCAL_DIR:-$HOME/eden-backups}"
REMOTE_ROOT=/var/lib/edenserver/backups
TS_PATTERN='[0-9]*T*Z'

echo "Backup source: $HOST:$REMOTE_ROOT"
echo "Pulling into: $LOCAL_ROOT"
echo

# --- Phase 1: discover worlds -----------------------------------------------
# "world" (the single-world layout) is timestamp dirs directly under
# REMOTE_ROOT; every other subdirectory name is a multi-world instance whose
# own timestamp dirs live one level down. Both layouts come from the same
# edenserverctl (see the instance-aware BACKUP_DIR default it picks).
world_list=$(ssh -n "$HOST" "
    set -eu
    root='$REMOTE_ROOT'
    ts='$TS_PATTERN'
    main_latest=\$(find \"\$root\" -maxdepth 1 -mindepth 1 -type d -name \"\$ts\" 2>/dev/null | sort -r | head -1)
    main_count=\$(find \"\$root\" -maxdepth 1 -mindepth 1 -type d -name \"\$ts\" 2>/dev/null | wc -l | tr -d ' ')
    printf 'world\t%s\t%s\n' \"\$main_count\" \"\$(basename \"\${main_latest:-none}\" 2>/dev/null || echo none)\"
    for d in \"\$root\"/*/; do
        [ -d \"\$d\" ] || continue
        name=\$(basename \"\$d\")
        case \"\$name\" in \$ts) continue ;; esac
        cnt=\$(find \"\$d\" -maxdepth 1 -mindepth 1 -type d -name \"\$ts\" 2>/dev/null | wc -l | tr -d ' ')
        [ \"\$cnt\" -gt 0 ] || continue
        latest=\$(find \"\$d\" -maxdepth 1 -mindepth 1 -type d -name \"\$ts\" 2>/dev/null | sort -r | head -1)
        printf '%s\t%s\t%s\n' \"\$name\" \"\$cnt\" \"\$(basename \"\$latest\")\"
    done
")

if [ -z "$world_list" ]; then
    echo "No backups found under $HOST:$REMOTE_ROOT — has a backup timer ever run?" >&2
    exit 1
fi

names=()
counts=()
latests=()
i=0
echo "Worlds with backups:"
while IFS=$'\t' read -r name count latest; do
    [ -n "$name" ] || continue
    names[i]="$name"
    counts[i]="$count"
    latests[i]="$latest"
    printf '  %d) %-20s %4s backup(s), latest %s\n' "$((i + 1))" "$name" "$count" "$latest"
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
world="${names[$((choice - 1))]}"

if [ "$world" = "world" ]; then
    remote_dir="$REMOTE_ROOT"
else
    remote_dir="$REMOTE_ROOT/$world"
fi

# --- Phase 2: list recent backups for the chosen world ----------------------
echo
echo "Most recent backups for '$world':"
backup_list=$(ssh -n "$HOST" "cd '$remote_dir' && find . -maxdepth 1 -mindepth 1 -type d -name '$TS_PATTERN' -printf '%f\n' | sort -r | head -n 15 | xargs -I{} du -sh {} 2>/dev/null")

stamps=()
sizes=()
i=0
while IFS=$'\t' read -r size stamp; do
    [ -n "$stamp" ] || continue
    stamps[i]="$stamp"
    sizes[i]="$size"
    printf '  %d) %s  (%s)\n' "$((i + 1))" "$stamp" "$size"
    i=$((i + 1))
done <<EOF
$backup_list
EOF

if [ "${#stamps[@]}" -eq 0 ]; then
    echo "No backups found for '$world'." >&2
    exit 1
fi

echo
read -r -p "Pick a backup [1-${#stamps[@]}, or 'latest']: " pick
if [ "$pick" = "latest" ] || [ "$pick" = "l" ]; then
    idx=0
elif [[ "$pick" =~ ^[0-9]+$ ]] && [ "$pick" -ge 1 ] && [ "$pick" -le "${#stamps[@]}" ]; then
    idx=$((pick - 1))
else
    echo "Not a valid choice." >&2
    exit 1
fi
stamp="${stamps[$idx]}"

# --- Phase 3: pull it down ---------------------------------------------------
dest="$LOCAL_ROOT/$world/$stamp"
mkdir -p "$dest"
echo
echo "Pulling $HOST:$remote_dir/$stamp/ -> $dest/"
rsync -avz --progress "$HOST:$remote_dir/$stamp/" "$dest/"

echo
echo "Done. $(ls "$dest" | wc -l | tr -d ' ') file(s) in $dest"
if [ "$world" = "world" ]; then
    echo "Restore on the VPS with:"
    echo "  edenserverctl stop"
    echo "  # for each *.gz: gunzip -c $dest/<name>.gz > /var/lib/edenserver/world/<name>"
    echo "  edenserverctl start"
else
    echo "Restore on the VPS with:"
    echo "  edenserverctl stop $world"
    echo "  # for each *.gz: gunzip -c $dest/<name>.gz > /var/lib/edenserver/worlds/$world/<name>"
    echo "  edenserverctl start $world"
fi
