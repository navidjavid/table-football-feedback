#!/usr/bin/env bash
#
# Snapshot the SQLite database into server/backups/ and prune anything
# older than 7 days. Designed for daily cron at 04:00.
#
# Crontab entry:
#   0 4 * * * /home/pi/server/scripts/backup_db.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
SERVER_DIR="$(cd -- "$SCRIPT_DIR/.." &>/dev/null && pwd)"
DB="$SERVER_DIR/football.db"
BACKUP_DIR="$SERVER_DIR/backups"

mkdir -p "$BACKUP_DIR"

if [[ ! -f "$DB" ]]; then
    echo "$(date -Is) backup_db: $DB does not exist, nothing to copy" >&2
    exit 0
fi

DATE="$(date +%Y%m%d_%H%M%S)"
DEST="$BACKUP_DIR/football_${DATE}.db"

# `sqlite3 .backup` takes a consistent snapshot even while the server is writing.
if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 "$DB" ".backup '$DEST'"
else
    cp -- "$DB" "$DEST"
fi

# Prune backups older than 7 days.
find "$BACKUP_DIR" -name "football_*.db" -type f -mtime +7 -delete

echo "$(date -Is) backup_db: wrote $DEST"
