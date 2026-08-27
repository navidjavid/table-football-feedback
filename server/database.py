"""All SQLite access lives here.

Rules:
  - SQLite connections are not shared across threads. We keep one
    connection per thread using `threading.local()`.
  - Every connection enables foreign keys and WAL mode.
  - Routes/MQTT handlers must call functions in this module — no raw SQL
    elsewhere in the codebase.
"""

from __future__ import annotations

import logging
import sqlite3
import threading
from datetime import datetime, timezone
from typing import Iterable

from config import DB_PATH, MAX_SCORE

log = logging.getLogger(__name__)

_local = threading.local()


def utc_now() -> str:
    """Server-side ISO-8601 UTC timestamp, second resolution."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def get_db() -> sqlite3.Connection:
    """Thread-local connection. Lazy-creates on first use per thread."""
    conn = getattr(_local, "conn", None)
    if conn is None:
        conn = sqlite3.connect(DB_PATH, timeout=10, isolation_level=None)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys = ON")
        conn.execute("PRAGMA journal_mode = WAL")
        conn.execute("PRAGMA synchronous = NORMAL")
        _local.conn = conn
    return conn


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

_SCHEMA = [
    """
    CREATE TABLE IF NOT EXISTS tables (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        location TEXT,
        max_score INTEGER NOT NULL DEFAULT 10,
        created_at TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS pico_devices (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        pico_id TEXT UNIQUE NOT NULL,
        table_id INTEGER REFERENCES tables(id),
        side TEXT,
        role TEXT,
        ip_address TEXT,
        firmware_version TEXT,
        last_seen TEXT,
        online INTEGER NOT NULL DEFAULT 0,
        created_at TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS players (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        rfid_uid TEXT UNIQUE NOT NULL,
        name TEXT NOT NULL,
        is_guest INTEGER NOT NULL DEFAULT 1,
        created_at TEXT NOT NULL,
        updated_at TEXT
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS live_tables (
        table_id INTEGER PRIMARY KEY REFERENCES tables(id),
        state TEXT NOT NULL DEFAULT 'WAITING',
        mode TEXT,
        score_a INTEGER NOT NULL DEFAULT 0,
        score_b INTEGER NOT NULL DEFAULT 0,
        max_score INTEGER NOT NULL DEFAULT 10,
        fastest_shot REAL DEFAULT 0,
        winner_side TEXT,
        winner_player_id INTEGER REFERENCES players(id),
        session_id TEXT,
        started_at TEXT,
        updated_at TEXT
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS live_players (
        table_id INTEGER NOT NULL REFERENCES tables(id),
        team_side TEXT NOT NULL,
        slot INTEGER NOT NULL,
        player_id INTEGER REFERENCES players(id),
        rfid_uid TEXT,
        source_pico_id TEXT,
        updated_at TEXT,
        PRIMARY KEY (table_id, team_side, slot)
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS matches (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        table_id INTEGER NOT NULL REFERENCES tables(id),
        mode TEXT NOT NULL,
        score_a INTEGER NOT NULL,
        score_b INTEGER NOT NULL,
        winner_side TEXT,
        winner_player_id INTEGER REFERENCES players(id),
        fastest_shot REAL,
        result_type TEXT NOT NULL DEFAULT 'completed',
        started_at TEXT,
        ended_at TEXT,
        session_id TEXT UNIQUE
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS match_players (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        match_id INTEGER NOT NULL REFERENCES matches(id),
        player_id INTEGER NOT NULL REFERENCES players(id),
        team_side TEXT NOT NULL,
        slot INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS rfid_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        table_id INTEGER REFERENCES tables(id),
        pico_id TEXT,
        team_side TEXT,
        slot INTEGER,
        rfid_uid TEXT NOT NULL,
        player_id INTEGER REFERENCES players(id),
        event_type TEXT,
        created_at TEXT NOT NULL
    )
    """,
    # Reserved for future tournament feature. No UI yet.
    """
    CREATE TABLE IF NOT EXISTS tournaments (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL,
        format TEXT NOT NULL DEFAULT 'single_elim',
        status TEXT NOT NULL DEFAULT 'pending',
        created_at TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS tournament_entries (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        tournament_id INTEGER NOT NULL REFERENCES tournaments(id),
        player_id INTEGER NOT NULL REFERENCES players(id),
        seed INTEGER
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS tournament_matches (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        tournament_id INTEGER NOT NULL REFERENCES tournaments(id),
        round INTEGER NOT NULL,
        bracket_position INTEGER NOT NULL,
        entry_a_id INTEGER REFERENCES tournament_entries(id),
        entry_b_id INTEGER REFERENCES tournament_entries(id),
        winner_entry_id INTEGER REFERENCES tournament_entries(id),
        table_id INTEGER REFERENCES tables(id),
        match_id INTEGER REFERENCES matches(id)
    )
    """,
]

# tournament_matches grew entry_a_id/entry_b_id/winner_entry_id/table_id
# after its first "reserved, no UI yet" version shipped with only
# (tournament_id, match_id, round, bracket_position). CREATE TABLE IF NOT
# EXISTS won't add columns to an already-created table, so patch it in
# for anyone who already has an old dev DB file lying around. No-op
# (raises "duplicate column", which we swallow) once already migrated.
_TOURNAMENT_MATCH_MIGRATIONS = [
    "ALTER TABLE tournament_matches ADD COLUMN entry_a_id INTEGER REFERENCES tournament_entries(id)",
    "ALTER TABLE tournament_matches ADD COLUMN entry_b_id INTEGER REFERENCES tournament_entries(id)",
    "ALTER TABLE tournament_matches ADD COLUMN winner_entry_id INTEGER REFERENCES tournament_entries(id)",
    "ALTER TABLE tournament_matches ADD COLUMN table_id INTEGER REFERENCES tables(id)",
]

_INDEXES = [
    "CREATE INDEX IF NOT EXISTS idx_matches_table_ended ON matches(table_id, ended_at)",
    "CREATE INDEX IF NOT EXISTS idx_match_players_player ON match_players(player_id)",
    "CREATE INDEX IF NOT EXISTS idx_match_players_match ON match_players(match_id)",
    "CREATE INDEX IF NOT EXISTS idx_rfid_events_table ON rfid_events(table_id, created_at)",
    "CREATE INDEX IF NOT EXISTS idx_live_players_table ON live_players(table_id)",
]


def init_db() -> None:
    """Create tables/indexes if missing and seed the default table."""
    db = get_db()
    for stmt in _SCHEMA:
        db.execute(stmt)
    for stmt in _TOURNAMENT_MATCH_MIGRATIONS:
        try:
            db.execute(stmt)
        except sqlite3.OperationalError as e:
            if "duplicate column" not in str(e).lower():
                raise
    for stmt in _INDEXES:
        db.execute(stmt)

    row = db.execute("SELECT COUNT(*) AS n FROM tables").fetchone()
    if row["n"] == 0:
        db.execute(
            "INSERT INTO tables (name, location, max_score, created_at) "
            "VALUES (?, ?, ?, ?)",
            ("Table 1", None, MAX_SCORE, utc_now()),
        )
        log.info("Seeded default Table 1")

    # Ensure every table has a live_tables row.
    db.execute(
        """
        INSERT OR IGNORE INTO live_tables (table_id, state, max_score, updated_at)
        SELECT id, 'WAITING', max_score, ? FROM tables
        """,
        (utc_now(),),
    )


def startup_reset() -> None:
    """Run on every Flask startup: reset transient state."""
    db = get_db()
    now = utc_now()
    db.execute(
        """
        UPDATE live_tables
           SET state = 'WAITING',
               mode = NULL,
               score_a = 0,
               score_b = 0,
               fastest_shot = 0,
               winner_side = NULL,
               winner_player_id = NULL,
               session_id = NULL,
               started_at = NULL,
               updated_at = ?
        """,
        (now,),
    )
    db.execute("DELETE FROM live_players")
    db.execute("UPDATE pico_devices SET online = 0")


# ---------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------

def list_tables() -> list[sqlite3.Row]:
    return get_db().execute(
        "SELECT * FROM tables ORDER BY id"
    ).fetchall()


def get_table(table_id: int) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM tables WHERE id = ?", (table_id,)
    ).fetchone()


def create_table(name: str, location: str | None) -> int:
    db = get_db()
    cur = db.execute(
        "INSERT INTO tables (name, location, max_score, created_at) "
        "VALUES (?, ?, ?, ?)",
        (name, location, MAX_SCORE, utc_now()),
    )
    table_id = cur.lastrowid
    db.execute(
        "INSERT INTO live_tables (table_id, state, max_score, updated_at) "
        "VALUES (?, 'WAITING', ?, ?)",
        (table_id, MAX_SCORE, utc_now()),
    )
    return table_id


def rename_table(table_id: int, name: str) -> None:
    get_db().execute("UPDATE tables SET name = ? WHERE id = ?", (name, table_id))


def set_table_max_score(table_id: int, max_score: int) -> None:
    db = get_db()
    db.execute("UPDATE tables SET max_score = ? WHERE id = ?", (max_score, table_id))
    db.execute(
        "UPDATE live_tables SET max_score = ?, updated_at = ? WHERE table_id = ?",
        (max_score, utc_now(), table_id),
    )


# ---------------------------------------------------------------------------
# Pico devices
# ---------------------------------------------------------------------------

def upsert_pico_from_heartbeat(
    pico_id: str,
    table_id: int | None,
    side: str | None,
    role: str | None,
    ip: str | None,
    firmware: str | None,
) -> None:
    """Insert or update a Pico row from a heartbeat payload."""
    db = get_db()
    now = utc_now()
    existing = db.execute(
        "SELECT id FROM pico_devices WHERE pico_id = ?", (pico_id,)
    ).fetchone()
    if existing:
        db.execute(
            """
            UPDATE pico_devices
               SET table_id = COALESCE(?, table_id),
                   side = COALESCE(?, side),
                   role = COALESCE(?, role),
                   ip_address = COALESCE(?, ip_address),
                   firmware_version = COALESCE(?, firmware_version),
                   last_seen = ?,
                   online = 1
             WHERE pico_id = ?
            """,
            (table_id, side, role, ip, firmware, now, pico_id),
        )
    else:
        db.execute(
            """
            INSERT INTO pico_devices
              (pico_id, table_id, side, role, ip_address, firmware_version,
               last_seen, online, created_at)
              VALUES (?, ?, ?, ?, ?, ?, ?, 1, ?)
            """,
            (pico_id, table_id, side, role, ip, firmware, now, now),
        )


def set_pico_online(pico_id: str, online: bool) -> None:
    get_db().execute(
        "UPDATE pico_devices SET online = ? WHERE pico_id = ?",
        (1 if online else 0, pico_id),
    )


def list_picos() -> list[sqlite3.Row]:
    return get_db().execute(
        "SELECT * FROM pico_devices ORDER BY pico_id"
    ).fetchall()


def get_pico(pico_id: str) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM pico_devices WHERE pico_id = ?", (pico_id,)
    ).fetchone()


def stale_picos(cutoff_iso: str) -> list[str]:
    rows = get_db().execute(
        "SELECT pico_id FROM pico_devices WHERE online = 1 AND last_seen < ?",
        (cutoff_iso,),
    ).fetchall()
    return [r["pico_id"] for r in rows]


def assign_pico(pico_id: str, table_id: int, side: str, role: str) -> None:
    get_db().execute(
        """
        UPDATE pico_devices
           SET table_id = ?, side = ?, role = ?
         WHERE pico_id = ?
        """,
        (table_id, side, role, pico_id),
    )


# ---------------------------------------------------------------------------
# Players
# ---------------------------------------------------------------------------

def get_player(player_id: int) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM players WHERE id = ?", (player_id,)
    ).fetchone()


def get_player_by_uid(uid: str) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM players WHERE rfid_uid = ?", (uid,)
    ).fetchone()


def _next_guest_name() -> str:
    """Pick the next free GuestN name."""
    row = get_db().execute(
        """
        SELECT name FROM players
         WHERE name LIKE 'Guest%'
         ORDER BY id DESC
         LIMIT 1
        """
    ).fetchone()
    next_n = 1
    if row:
        # Find highest GuestN among ALL guests, not just the last one.
        all_guests = get_db().execute(
            "SELECT name FROM players WHERE name LIKE 'Guest%'"
        ).fetchall()
        used = []
        for r in all_guests:
            tail = r["name"][len("Guest"):]
            if tail.isdigit():
                used.append(int(tail))
        next_n = (max(used) + 1) if used else 1
    return f"Guest{next_n}"


def get_or_create_player_by_uid(uid: str) -> sqlite3.Row:
    """Find player by RFID UID, or create a Guest with the next free number."""
    existing = get_player_by_uid(uid)
    if existing:
        return existing
    name = _next_guest_name()
    now = utc_now()
    db = get_db()
    db.execute(
        "INSERT INTO players (rfid_uid, name, is_guest, created_at, updated_at) "
        "VALUES (?, ?, 1, ?, ?)",
        (uid, name, now, now),
    )
    log.info("Created guest player %s for UID %s", name, uid)
    return get_player_by_uid(uid)  # type: ignore[return-value]


def rename_player(player_id: int, new_name: str, register: bool) -> None:
    """Update name (and optionally clear is_guest). Same row, history intact."""
    fields = ["name = ?", "updated_at = ?"]
    params: list = [new_name, utc_now()]
    if register:
        fields.append("is_guest = 0")
    params.append(player_id)
    get_db().execute(
        f"UPDATE players SET {', '.join(fields)} WHERE id = ?", params
    )


def name_collides(name: str, excluding_id: int) -> bool:
    """Return True if another registered (non-guest) player owns this name."""
    row = get_db().execute(
        """
        SELECT id FROM players
         WHERE LOWER(name) = LOWER(?)
           AND is_guest = 0
           AND id != ?
         LIMIT 1
        """,
        (name, excluding_id),
    ).fetchone()
    return row is not None


def list_players_with_stats() -> list[dict]:
    """All players, with derived games/wins/losses/best_shot/status."""
    db = get_db()
    rows = db.execute("SELECT * FROM players ORDER BY name").fetchall()

    # Players who are currently sitting at a table.
    seated = {
        r["player_id"]: r["state"]
        for r in db.execute(
            """
            SELECT lp.player_id, lt.state
              FROM live_players lp
              JOIN live_tables lt ON lt.table_id = lp.table_id
             WHERE lp.player_id IS NOT NULL
            """
        ).fetchall()
    }

    result = []
    for r in rows:
        stats = _player_stats(r["id"])
        if r["id"] in seated:
            status = "Active" if seated[r["id"]] == "GAME_PLAYING" else "Waiting"
        else:
            status = "Offline"
        result.append({
            "id": r["id"],
            "name": r["name"],
            "rfid_uid": r["rfid_uid"],
            "is_guest": bool(r["is_guest"]),
            "games": stats["games"],
            "wins": stats["wins"],
            "losses": stats["losses"],
            "best_shot": stats["best_shot"],
            "status": status,
        })
    return result


def _player_stats(player_id: int) -> dict:
    db = get_db()
    games = db.execute(
        "SELECT COUNT(DISTINCT match_id) AS n FROM match_players WHERE player_id = ?",
        (player_id,),
    ).fetchone()["n"]
    wins = db.execute(
        """
        SELECT COUNT(DISTINCT m.id) AS n
          FROM matches m
          JOIN match_players mp ON mp.match_id = m.id
         WHERE mp.player_id = ?
           AND m.result_type = 'completed'
           AND m.winner_side = mp.team_side
        """,
        (player_id,),
    ).fetchone()["n"]
    best = db.execute(
        """
        SELECT MAX(m.fastest_shot) AS best
          FROM matches m
          JOIN match_players mp ON mp.match_id = m.id
         WHERE mp.player_id = ?
        """,
        (player_id,),
    ).fetchone()["best"]
    return {
        "games": games,
        "wins": wins,
        "losses": max(0, games - wins),
        "best_shot": float(best) if best is not None else 0.0,
    }


def player_match_history(player_id: int, limit: int = 20) -> list[dict]:
    """Recent matches the player participated in (newest first)."""
    db = get_db()
    rows = db.execute(
        """
        SELECT m.*, mp.team_side AS my_side
          FROM matches m
          JOIN match_players mp ON mp.match_id = m.id
         WHERE mp.player_id = ?
         ORDER BY COALESCE(m.ended_at, m.started_at) DESC
         LIMIT ?
        """,
        (player_id, limit),
    ).fetchall()

    out = []
    for m in rows:
        teammates = db.execute(
            """
            SELECT p.name FROM match_players mp
              JOIN players p ON p.id = mp.player_id
             WHERE mp.match_id = ? AND mp.team_side = ? AND mp.player_id != ?
             ORDER BY mp.slot
            """,
            (m["id"], m["my_side"], player_id),
        ).fetchall()
        opp_side = "B" if m["my_side"] == "A" else "A"
        opponents = db.execute(
            """
            SELECT p.name FROM match_players mp
              JOIN players p ON p.id = mp.player_id
             WHERE mp.match_id = ? AND mp.team_side = ?
             ORDER BY mp.slot
            """,
            (m["id"], opp_side),
        ).fetchall()

        if m["result_type"] == "abandoned":
            result = "Abandoned"
        elif m["winner_side"] == m["my_side"]:
            result = "Win"
        else:
            result = "Loss"

        my_score = m["score_a"] if m["my_side"] == "A" else m["score_b"]
        opp_score = m["score_b"] if m["my_side"] == "A" else m["score_a"]

        out.append({
            "match_id": m["id"],
            "table_id": m["table_id"],
            "mode": m["mode"],
            "date": m["ended_at"] or m["started_at"],
            "teammates": [t["name"] for t in teammates],
            "opponents": [o["name"] for o in opponents],
            "score": f"{my_score} - {opp_score}",
            "result": result,
            "fastest_shot": float(m["fastest_shot"] or 0.0),
        })
    return out


# ---------------------------------------------------------------------------
# Live tables / live players
# ---------------------------------------------------------------------------

def get_live_table(table_id: int) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM live_tables WHERE table_id = ?", (table_id,)
    ).fetchone()


def update_live_table(table_id: int, **fields) -> None:
    if not fields:
        return
    fields["updated_at"] = utc_now()
    keys = ", ".join(f"{k} = ?" for k in fields)
    params = list(fields.values()) + [table_id]
    get_db().execute(
        f"UPDATE live_tables SET {keys} WHERE table_id = ?", params
    )


def get_live_players(table_id: int) -> list[sqlite3.Row]:
    return get_db().execute(
        """
        SELECT lp.*, p.name AS player_name, p.is_guest AS is_guest
          FROM live_players lp
          LEFT JOIN players p ON p.id = lp.player_id
         WHERE lp.table_id = ?
         ORDER BY lp.team_side, lp.slot
        """,
        (table_id,),
    ).fetchall()


def find_live_player_by_uid(table_id: int, uid: str) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM live_players WHERE table_id = ? AND rfid_uid = ?",
        (table_id, uid),
    ).fetchone()


def first_empty_slot(table_id: int, side: str) -> int | None:
    used = {
        r["slot"] for r in get_db().execute(
            "SELECT slot FROM live_players WHERE table_id = ? AND team_side = ?",
            (table_id, side),
        ).fetchall()
    }
    for slot in (1, 2):
        if slot not in used:
            return slot
    return None


def add_live_player(
    table_id: int, side: str, slot: int,
    player_id: int, uid: str, pico_id: str | None,
) -> None:
    get_db().execute(
        """
        INSERT OR REPLACE INTO live_players
          (table_id, team_side, slot, player_id, rfid_uid, source_pico_id, updated_at)
          VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        (table_id, side, slot, player_id, uid, pico_id, utc_now()),
    )


def remove_live_player(table_id: int, side: str, slot: int) -> None:
    get_db().execute(
        "DELETE FROM live_players WHERE table_id = ? AND team_side = ? AND slot = ?",
        (table_id, side, slot),
    )


def compact_slots(table_id: int, side: str) -> None:
    """If slot 1 is empty but slot 2 is occupied, move slot 2 → slot 1."""
    rows = get_db().execute(
        "SELECT * FROM live_players WHERE table_id = ? AND team_side = ? ORDER BY slot",
        (table_id, side),
    ).fetchall()
    if len(rows) == 1 and rows[0]["slot"] == 2:
        r = rows[0]
        db = get_db()
        db.execute(
            "DELETE FROM live_players WHERE table_id = ? AND team_side = ? AND slot = 2",
            (table_id, side),
        )
        db.execute(
            """
            INSERT INTO live_players
              (table_id, team_side, slot, player_id, rfid_uid, source_pico_id, updated_at)
              VALUES (?, ?, 1, ?, ?, ?, ?)
            """,
            (table_id, side, r["player_id"], r["rfid_uid"], r["source_pico_id"], utc_now()),
        )


def clear_live_players(table_id: int) -> None:
    get_db().execute("DELETE FROM live_players WHERE table_id = ?", (table_id,))


def count_side(table_id: int, side: str) -> int:
    return get_db().execute(
        "SELECT COUNT(*) AS n FROM live_players WHERE table_id = ? AND team_side = ?",
        (table_id, side),
    ).fetchone()["n"]


# ---------------------------------------------------------------------------
# Matches
# ---------------------------------------------------------------------------

def save_match(
    table_id: int,
    mode: str,
    score_a: int,
    score_b: int,
    winner_side: str | None,
    winner_player_id: int | None,
    fastest_shot: float | None,
    result_type: str,
    started_at: str | None,
    session_id: str | None,
    players: Iterable[dict],
) -> int | None:
    """Save a finished or abandoned match. Deduplicates on session_id."""
    db = get_db()
    if session_id:
        dup = db.execute(
            "SELECT id FROM matches WHERE session_id = ?", (session_id,)
        ).fetchone()
        if dup:
            log.info("Duplicate match for session %s — skipped", session_id)
            return None
    try:
        cur = db.execute(
            """
            INSERT INTO matches
              (table_id, mode, score_a, score_b, winner_side, winner_player_id,
               fastest_shot, result_type, started_at, ended_at, session_id)
              VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                table_id, mode, score_a, score_b, winner_side, winner_player_id,
                fastest_shot, result_type, started_at, utc_now(), session_id,
            ),
        )
    except sqlite3.IntegrityError:
        log.info("Match insert race — session %s already saved", session_id)
        return None

    match_id = cur.lastrowid
    for p in players:
        db.execute(
            """
            INSERT INTO match_players (match_id, player_id, team_side, slot)
              VALUES (?, ?, ?, ?)
            """,
            (match_id, p["player_id"], p["team_side"], p["slot"]),
        )
    log.info("Saved match #%s (%s) on table %s", match_id, result_type, table_id)
    return match_id


def list_matches(table_id: int | None = None,
                 player_id: int | None = None,
                 limit: int = 50) -> list[dict]:
    db = get_db()
    where = []
    params: list = []
    if table_id is not None:
        where.append("m.table_id = ?")
        params.append(table_id)
    sql = "SELECT m.* FROM matches m"
    if player_id is not None:
        sql += " JOIN match_players mp ON mp.match_id = m.id"
        where.append("mp.player_id = ?")
        params.append(player_id)
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY COALESCE(m.ended_at, m.started_at) DESC LIMIT ?"
    params.append(limit)

    rows = db.execute(sql, params).fetchall()

    out = []
    for m in rows:
        side_a = db.execute(
            """
            SELECT p.id, p.name, p.is_guest, mp.slot
              FROM match_players mp JOIN players p ON p.id = mp.player_id
             WHERE mp.match_id = ? AND mp.team_side = 'A'
             ORDER BY mp.slot
            """,
            (m["id"],),
        ).fetchall()
        side_b = db.execute(
            """
            SELECT p.id, p.name, p.is_guest, mp.slot
              FROM match_players mp JOIN players p ON p.id = mp.player_id
             WHERE mp.match_id = ? AND mp.team_side = 'B'
             ORDER BY mp.slot
            """,
            (m["id"],),
        ).fetchall()
        out.append({
            "id": m["id"],
            "table_id": m["table_id"],
            "mode": m["mode"],
            "score_a": m["score_a"],
            "score_b": m["score_b"],
            "winner_side": m["winner_side"],
            "fastest_shot": float(m["fastest_shot"] or 0.0),
            "result_type": m["result_type"],
            "started_at": m["started_at"],
            "ended_at": m["ended_at"],
            "team_a": [dict(r) for r in side_a],
            "team_b": [dict(r) for r in side_b],
        })
    return out


# ---------------------------------------------------------------------------
# RFID events
# ---------------------------------------------------------------------------

def log_rfid_event(
    table_id: int | None,
    pico_id: str | None,
    side: str | None,
    slot: int | None,
    uid: str,
    player_id: int | None,
    event_type: str,
) -> None:
    get_db().execute(
        """
        INSERT INTO rfid_events
          (table_id, pico_id, team_side, slot, rfid_uid, player_id, event_type, created_at)
          VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (table_id, pico_id, side, slot, uid, player_id, event_type, utc_now()),
    )


# ---------------------------------------------------------------------------
# Convenience: full snapshot for SSE / dashboard
# ---------------------------------------------------------------------------

def table_snapshot(table_id: int) -> dict | None:
    """Composite snapshot used by /api/table/<id>/live, SSE pushes, MQTT sync."""
    t = get_table(table_id)
    if not t:
        return None
    lt = get_live_table(table_id)
    lps = get_live_players(table_id)

    def player_blob(rows: list[sqlite3.Row], side: str) -> list[dict]:
        return [
            {
                "slot": r["slot"],
                "player_id": r["player_id"],
                "name": r["player_name"] or "",
                "uid": r["rfid_uid"],
                "is_guest": bool(r["is_guest"]) if r["is_guest"] is not None else True,
            }
            for r in rows if r["team_side"] == side
        ]

    return {
        "table_id": table_id,
        "name": t["name"],
        "state": lt["state"] if lt else "WAITING",
        "mode": lt["mode"] if lt else None,
        "score_a": lt["score_a"] if lt else 0,
        "score_b": lt["score_b"] if lt else 0,
        "max_score": (lt["max_score"] if lt else t["max_score"]) or MAX_SCORE,
        "fastest": float(lt["fastest_shot"] or 0.0) if lt else 0.0,
        "winner_side": lt["winner_side"] if lt else None,
        "winner_player_id": lt["winner_player_id"] if lt else None,
        "session_id": lt["session_id"] if lt else None,
        "team_a": player_blob(lps, "A"),
        "team_b": player_blob(lps, "B"),
        "updated_at": lt["updated_at"] if lt else None,
    }


def all_table_snapshots() -> list[dict]:
    return [
        s for s in (table_snapshot(t["id"]) for t in list_tables()) if s
    ]


def registered_players_list() -> list[dict]:
    """{uid, name} for every player — sent to Picos for offline mode."""
    rows = get_db().execute(
        "SELECT rfid_uid, name FROM players ORDER BY id"
    ).fetchall()
    return [{"uid": r["rfid_uid"], "name": r["name"]} for r in rows]


# ---------------------------------------------------------------------------
# Tournaments (single-elimination bracket)
#
# An "entry" is one player occupying one bracket slot (2v2/team entries
# aren't modeled — each tournament participant is a single player, same
# granularity as `players`). A bracket is generated once from the entries
# at tournament-start time; byes (odd/non-power-of-2 entry counts) are
# auto-advanced immediately. Winners propagate round-to-round automatically
# via record_tournament_match_result(), called from mqtt_client.py whenever
# a live match tied to an open tournament_matches row finishes.
# ---------------------------------------------------------------------------

def create_tournament(name: str, fmt: str = "single_elim") -> int:
    db = get_db()
    cur = db.execute(
        "INSERT INTO tournaments (name, format, status, created_at) VALUES (?, ?, 'pending', ?)",
        (name, fmt, utc_now()),
    )
    return cur.lastrowid


def delete_tournament(tournament_id: int) -> None:
    """No ON DELETE CASCADE in the schema, so clear child rows first."""
    db = get_db()
    db.execute("DELETE FROM tournament_matches WHERE tournament_id = ?", (tournament_id,))
    db.execute("DELETE FROM tournament_entries WHERE tournament_id = ?", (tournament_id,))
    db.execute("DELETE FROM tournaments WHERE id = ?", (tournament_id,))


def list_tournaments() -> list[sqlite3.Row]:
    return get_db().execute(
        "SELECT * FROM tournaments ORDER BY id DESC"
    ).fetchall()


def get_tournament(tournament_id: int) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM tournaments WHERE id = ?", (tournament_id,)
    ).fetchone()


def add_tournament_entry(tournament_id: int, player_id: int, seed: int | None = None) -> int:
    db = get_db()
    cur = db.execute(
        "INSERT INTO tournament_entries (tournament_id, player_id, seed) VALUES (?, ?, ?)",
        (tournament_id, player_id, seed),
    )
    return cur.lastrowid


def list_tournament_entries(tournament_id: int) -> list[sqlite3.Row]:
    return get_db().execute(
        """
        SELECT te.*, p.name AS player_name, p.rfid_uid
        FROM tournament_entries te JOIN players p ON p.id = te.player_id
        WHERE te.tournament_id = ?
        ORDER BY te.seed IS NULL, te.seed, te.id
        """,
        (tournament_id,),
    ).fetchall()


def _tm_row(tm_id: int) -> sqlite3.Row | None:
    return get_db().execute(
        "SELECT * FROM tournament_matches WHERE id = ?", (tm_id,)
    ).fetchone()


def generate_bracket(tournament_id: int) -> None:
    """Creates round-1 matches from the current entries and auto-advances
    any byes. Raises ValueError if there are fewer than 2 entries or the
    tournament already has a bracket."""
    db = get_db()
    existing = db.execute(
        "SELECT COUNT(*) AS n FROM tournament_matches WHERE tournament_id = ?",
        (tournament_id,),
    ).fetchone()["n"]
    if existing:
        raise ValueError("bracket already generated for this tournament")

    entries = list_tournament_entries(tournament_id)
    n = len(entries)
    if n < 2:
        raise ValueError("need at least 2 entries to start a bracket")

    size = 1
    while size < n:
        size *= 2
    num_byes = size - n

    # Byes go to the top seeds (entries are already ordered by seed, then
    # insertion order) and each gets its own match slot paired against
    # nothing — never two byes sharing a match, which would create an
    # empty slot that can never produce a winner to advance.
    bye_entries = entries[:num_byes]
    paired_entries = entries[num_byes:]

    pairs: list[tuple[int, int | None]] = [(e["id"], None) for e in bye_entries]
    for i in range(0, len(paired_entries), 2):
        pairs.append((paired_entries[i]["id"], paired_entries[i + 1]["id"]))

    new_ids = []
    for i, (a, b) in enumerate(pairs):
        cur = db.execute(
            "INSERT INTO tournament_matches "
            "(tournament_id, round, bracket_position, entry_a_id, entry_b_id) "
            "VALUES (?, 1, ?, ?, ?)",
            (tournament_id, i, a, b),
        )
        new_ids.append(cur.lastrowid)

    db.execute("UPDATE tournaments SET status = 'active' WHERE id = ?", (tournament_id,))
    for tm_id in new_ids:
        _maybe_auto_advance_bye(tournament_id, tm_id)


def _maybe_auto_advance_bye(tournament_id: int, tm_id: int) -> None:
    tm = _tm_row(tm_id)
    if not tm or tm["winner_entry_id"] is not None:
        return
    # Byes only ever exist by construction in round 1 (generate_bracket()
    # is the only place a match is deliberately created with one slot
    # permanently null). A round > 1 match with only one slot filled is
    # NOT a bye — it's a real match still waiting on its other feeder
    # match to actually be played. Auto-advancing it here would let one
    # bye-recipient skip an entire round without ever facing an opponent.
    if tm["round"] != 1:
        return
    has_a, has_b = tm["entry_a_id"] is not None, tm["entry_b_id"] is not None
    if has_a and not has_b:
        _advance_winner(tournament_id, tm, tm["entry_a_id"], None)
    elif has_b and not has_a:
        _advance_winner(tournament_id, tm, tm["entry_b_id"], None)


def _advance_winner(tournament_id: int, tm: sqlite3.Row, winner_entry_id: int,
                    match_id: int | None) -> None:
    db = get_db()
    db.execute(
        "UPDATE tournament_matches SET winner_entry_id = ?, match_id = ? WHERE id = ?",
        (winner_entry_id, match_id, tm["id"]),
    )

    round_size = db.execute(
        "SELECT COUNT(*) AS n FROM tournament_matches WHERE tournament_id = ? AND round = ?",
        (tournament_id, tm["round"]),
    ).fetchone()["n"]
    if round_size == 1:
        db.execute("UPDATE tournaments SET status = 'completed' WHERE id = ?", (tournament_id,))
        return

    next_round = tm["round"] + 1
    next_pos = tm["bracket_position"] // 2
    slot_col = "entry_a_id" if tm["bracket_position"] % 2 == 0 else "entry_b_id"

    next_row = db.execute(
        "SELECT * FROM tournament_matches WHERE tournament_id = ? AND round = ? AND bracket_position = ?",
        (tournament_id, next_round, next_pos),
    ).fetchone()
    if next_row is None:
        cur = db.execute(
            f"INSERT INTO tournament_matches (tournament_id, round, bracket_position, {slot_col}) "
            f"VALUES (?, ?, ?, ?)",
            (tournament_id, next_round, next_pos, winner_entry_id),
        )
        new_id = cur.lastrowid
    else:
        db.execute(
            f"UPDATE tournament_matches SET {slot_col} = ? WHERE id = ?",
            (winner_entry_id, next_row["id"]),
        )
        new_id = next_row["id"]

    _maybe_auto_advance_bye(tournament_id, new_id)


def record_tournament_match_result(tm_id: int, winner_entry_id: int,
                                   match_id: int | None) -> None:
    """Called once a live match tied to this bracket slot finishes."""
    tm = _tm_row(tm_id)
    if not tm or tm["winner_entry_id"] is not None:
        return
    _advance_winner(tm["tournament_id"], tm, winner_entry_id, match_id)


def find_open_tournament_match_by_table(table_id: int) -> sqlite3.Row | None:
    """The most recent bracket slot assigned to this table that hasn't
    been decided yet — used to detect that a just-finished live match was
    actually a tournament match."""
    return get_db().execute(
        "SELECT * FROM tournament_matches WHERE table_id = ? AND winner_entry_id IS NULL "
        "ORDER BY id DESC LIMIT 1",
        (table_id,),
    ).fetchone()


def assign_tournament_match_to_table(tm_id: int, table_id: int) -> None:
    get_db().execute(
        "UPDATE tournament_matches SET table_id = ? WHERE id = ?", (table_id, tm_id)
    )


def get_bracket(tournament_id: int) -> list[dict]:
    """Every round's matches with entry names resolved, for a future
    bracket UI (or just JSON inspection) to render."""
    rows = get_db().execute(
        """
        SELECT tm.*,
               pa.name AS entry_a_name, pb.name AS entry_b_name,
               pw.name AS winner_name
        FROM tournament_matches tm
        LEFT JOIN tournament_entries ea ON ea.id = tm.entry_a_id
        LEFT JOIN tournament_entries eb ON eb.id = tm.entry_b_id
        LEFT JOIN tournament_entries ew ON ew.id = tm.winner_entry_id
        LEFT JOIN players pa ON pa.id = ea.player_id
        LEFT JOIN players pb ON pb.id = eb.player_id
        LEFT JOIN players pw ON pw.id = ew.player_id
        WHERE tm.tournament_id = ?
        ORDER BY tm.round, tm.bracket_position
        """,
        (tournament_id,),
    ).fetchall()
    return [dict(r) for r in rows]
