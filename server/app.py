"""Flask app — public dashboard, REST API, admin panel, SSE stream.

Startup wires up:
  1. DB schema + idempotent migrations
  2. Reset of transient live state
  3. MQTT client (background thread)
  4. Heartbeat watchdog (background thread)

Run directly with `python app.py` for development. In production the
systemd unit invokes the same entry point.
"""

from __future__ import annotations

import json
import logging
import queue
import threading
import time
from datetime import datetime, timedelta, timezone
from functools import wraps

from flask import (
    Flask, Response, abort, jsonify, redirect, render_template, request, session,
    stream_with_context, url_for,
)

import database as db
import mqtt_client
import state
from config import (
    ADMIN_PASSWORD, BASE_DIR, PICO_OFFLINE_SEC, SECRET_KEY, configure_logging,
)

configure_logging()
log = logging.getLogger(__name__)

app = Flask(
    __name__,
    template_folder=str(BASE_DIR / "templates"),
    static_folder=str(BASE_DIR / "static"),
)
app.secret_key = SECRET_KEY


# ---------------------------------------------------------------------------
# Admin auth helpers
# ---------------------------------------------------------------------------

_login_attempts: dict[str, list[float]] = {}
_login_lock = threading.Lock()


def _rate_limit_login(ip: str) -> bool:
    """Return True if this IP is over the 5-attempts-per-minute cap."""
    now = time.monotonic()
    with _login_lock:
        hist = [t for t in _login_attempts.get(ip, []) if now - t < 60]
        hist.append(now)
        _login_attempts[ip] = hist
        return len(hist) > 5


def admin_required(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        if not session.get("admin"):
            if request.path.startswith("/api/"):
                return jsonify({"error": "unauthorized"}), 401
            return redirect(url_for("admin_login_page"))
        return fn(*args, **kwargs)
    return wrapper


# ---------------------------------------------------------------------------
# Public pages
# ---------------------------------------------------------------------------

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/player/<uid>")
def player_page(uid: str):
    return render_template("player.html", uid=uid.upper())


# ---------------------------------------------------------------------------
# SSE
# ---------------------------------------------------------------------------

@app.route("/events")
def events():
    @stream_with_context
    def stream():
        q = state.register_sse_client()
        # Send the current snapshot of every table so a new connection
        # paints something useful before any change events arrive.
        for snap in state.all_snapshots():
            yield f"event: table_state\ndata: {json.dumps(snap, separators=(',', ':'))}\n\n"
        try:
            while True:
                try:
                    msg = q.get(timeout=15)
                    yield msg
                except queue.Empty:
                    # Keep-alive comment line to keep the connection open
                    # through proxies/timeouts.
                    yield ": keepalive\n\n"
        except GeneratorExit:
            pass
        finally:
            state.unregister_sse_client(q)

    return Response(
        stream(),
        mimetype="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        },
    )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

@app.route("/api/tables")
def api_tables():
    return jsonify(db.all_table_snapshots())


@app.route("/api/table/<int:table_id>/live")
def api_table_live(table_id: int):
    snap = db.table_snapshot(table_id)
    if not snap:
        abort(404)
    return jsonify(snap)


@app.route("/api/players")
def api_players():
    return jsonify(db.list_players_with_stats())


@app.route("/api/player/<int:player_id>")
def api_player(player_id: int):
    p = db.get_player(player_id)
    if not p:
        abort(404)
    stats = db._player_stats(player_id)
    return jsonify({
        "id": p["id"],
        "name": p["name"],
        "rfid_uid": p["rfid_uid"],
        "is_guest": bool(p["is_guest"]),
        **stats,
        "matches": db.player_match_history(player_id, limit=20),
    })


@app.route("/api/player/by-uid/<uid>")
def api_player_by_uid(uid: str):
    p = db.get_player_by_uid(uid.upper())
    if not p:
        abort(404)
    return api_player(p["id"])


@app.route("/api/matches")
def api_matches():
    table_id = request.args.get("table_id", type=int)
    player_id = request.args.get("player_id", type=int)
    limit = request.args.get("limit", default=50, type=int)
    return jsonify(db.list_matches(table_id=table_id, player_id=player_id,
                                   limit=min(max(limit, 1), 200)))


@app.route("/api/register-player", methods=["POST"])
def api_register_player():
    data = request.get_json(silent=True) or {}
    try:
        player_id = int(data.get("player_id"))
    except (TypeError, ValueError):
        return jsonify({"error": "player_id required"}), 400
    name = (data.get("name") or "").strip()
    if not name:
        return jsonify({"error": "name required"}), 400
    if len(name) > 40:
        return jsonify({"error": "name too long"}), 400

    p = db.get_player(player_id)
    if not p:
        return jsonify({"error": "player not found"}), 404

    if db.name_collides(name, excluding_id=player_id):
        return jsonify({"error": f"another player is already named {name!r}"}), 400

    db.rename_player(player_id, name, register=True)

    # Refresh affected table snapshots so the dashboard updates immediately.
    for snap in db.all_table_snapshots():
        if any(t["player_id"] == player_id for t in snap["team_a"] + snap["team_b"]):
            state.set_table_snapshot(snap["table_id"], snap)
            state.broadcast("table_state", snap)
    state.broadcast("player_update", {"players": db.list_players_with_stats()})

    fresh = db.get_player(player_id)
    return jsonify({
        "id": fresh["id"],
        "name": fresh["name"],
        "rfid_uid": fresh["rfid_uid"],
        "is_guest": bool(fresh["is_guest"]),
    })


# ---------------------------------------------------------------------------
# Admin pages
# ---------------------------------------------------------------------------

@app.route("/admin/login")
def admin_login_page():
    return render_template("admin_login.html")


@app.route("/admin")
@admin_required
def admin_page():
    return render_template("admin.html")


@app.route("/api/admin/login", methods=["POST"])
def admin_login():
    ip = request.remote_addr or "?"
    if _rate_limit_login(ip):
        log.warning("Login rate limit hit from %s", ip)
        return jsonify({"error": "too many attempts, try again in a minute"}), 429

    data = request.get_json(silent=True) or {}
    if data.get("password") == ADMIN_PASSWORD:
        session["admin"] = True
        return jsonify({"ok": True})
    log.warning("Failed admin login from %s", ip)
    return jsonify({"error": "invalid password"}), 401


@app.route("/api/admin/logout", methods=["POST"])
def admin_logout():
    session.pop("admin", None)
    return jsonify({"ok": True})


# ---------------------------------------------------------------------------
# Admin API
# ---------------------------------------------------------------------------

@app.route("/api/admin/picos")
@admin_required
def api_admin_picos():
    out = []
    statuses = state.list_pico_statuses()
    for row in db.list_picos():
        s = statuses.get(row["pico_id"], {})
        out.append({
            "pico_id": row["pico_id"],
            "table_id": row["table_id"],
            "side": row["side"],
            "role": row["role"],
            "ip_address": row["ip_address"],
            "firmware_version": row["firmware_version"],
            "last_seen": row["last_seen"],
            "online": bool(row["online"]) or s.get("online", False),
        })
    return jsonify(out)


@app.route("/api/admin/pico/<pico_id>/command", methods=["POST"])
@admin_required
def api_admin_pico_command(pico_id: str):
    data = request.get_json(silent=True) or {}
    cmd = data.get("cmd")
    if cmd not in ("identify", "reset_match", "clear_players", "sync_players", "show_message"):
        return jsonify({"error": "unknown command"}), 400

    if cmd == "sync_players":
        mqtt_client.publish_players_list(pico_id)
        return jsonify({"ok": True})

    payload = {"cmd": cmd}
    if cmd == "show_message":
        msg = (data.get("message") or "").strip()
        if not msg:
            return jsonify({"error": "message required"}), 400
        payload["message"] = msg[:64]
    mqtt_client.publish_cmd(pico_id, payload)
    return jsonify({"ok": True})


@app.route("/api/admin/pico/<pico_id>/assign", methods=["POST"])
@admin_required
def api_admin_pico_assign(pico_id: str):
    data = request.get_json(silent=True) or {}
    try:
        table_id = int(data["table_id"])
    except (KeyError, TypeError, ValueError):
        return jsonify({"error": "table_id required"}), 400
    side = data.get("side")
    role = data.get("role")
    if side not in ("A", "B") or role not in ("primary", "secondary"):
        return jsonify({"error": "invalid side or role"}), 400
    if not db.get_table(table_id):
        return jsonify({"error": "table not found"}), 404
    db.assign_pico(pico_id, table_id, side, role)
    return jsonify({"ok": True})


@app.route("/api/admin/table", methods=["POST"])
@admin_required
def api_admin_create_table():
    data = request.get_json(silent=True) or {}
    name = (data.get("name") or "").strip()
    if not name:
        return jsonify({"error": "name required"}), 400
    location = (data.get("location") or "").strip() or None
    tid = db.create_table(name, location)
    snap = db.table_snapshot(tid)
    if snap:
        state.set_table_snapshot(tid, snap)
        state.broadcast("table_state", snap)
    return jsonify({"ok": True, "table_id": tid})


@app.route("/api/admin/table/<int:table_id>/rename", methods=["POST"])
@admin_required
def api_admin_rename_table(table_id: int):
    name = ((request.get_json(silent=True) or {}).get("name") or "").strip()
    if not name:
        return jsonify({"error": "name required"}), 400
    if not db.get_table(table_id):
        abort(404)
    db.rename_table(table_id, name)
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/table/<int:table_id>/max_score", methods=["POST"])
@admin_required
def api_admin_max_score(table_id: int):
    data = request.get_json(silent=True) or {}
    try:
        max_score = int(data["max_score"])
    except (KeyError, TypeError, ValueError):
        return jsonify({"error": "max_score required"}), 400
    if not (1 <= max_score <= 99):
        return jsonify({"error": "max_score must be 1..99"}), 400
    if not db.get_table(table_id):
        abort(404)
    db.set_table_max_score(table_id, max_score)
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/table/<int:table_id>/reset", methods=["POST"])
@admin_required
def api_admin_reset_table(table_id: int):
    if not db.get_table(table_id):
        abort(404)
    db.clear_live_players(table_id)
    db.update_live_table(
        table_id,
        state="WAITING",
        mode=None,
        score_a=0,
        score_b=0,
        fastest_shot=0,
        winner_side=None,
        winner_player_id=None,
        session_id=None,
        started_at=None,
    )
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/table/<int:table_id>/start", methods=["POST"])
@admin_required
def api_admin_start(table_id: int):
    lt = db.get_live_table(table_id)
    if not lt:
        abort(404)
    team_a = [r for r in db.get_live_players(table_id) if r["team_side"] == "A"]
    team_b = [r for r in db.get_live_players(table_id) if r["team_side"] == "B"]
    if not (any(r["slot"] == 1 for r in team_a) and any(r["slot"] == 1 for r in team_b)):
        return jsonify({"error": "both sides need at least slot 1 filled"}), 400
    db.update_live_table(
        table_id,
        state="GAME_PLAYING",
        score_a=0,
        score_b=0,
        fastest_shot=0,
        winner_side=None,
        winner_player_id=None,
        started_at=db.utc_now(),
    )
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/table/<int:table_id>/stop", methods=["POST"])
@admin_required
def api_admin_stop(table_id: int):
    lt = db.get_live_table(table_id)
    if not lt:
        abort(404)
    if lt["state"] == "GAME_PLAYING":
        mqtt_client._abandon_match(table_id, reason="admin stop")  # noqa: SLF001
    else:
        db.update_live_table(table_id, state="WAITING", mode=None, started_at=None)
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/table/<int:table_id>/add_player", methods=["POST"])
@admin_required
def api_admin_add_player(table_id: int):
    data = request.get_json(silent=True) or {}
    side = data.get("side")
    try:
        slot = int(data["slot"])
        player_id = int(data["player_id"])
    except (KeyError, TypeError, ValueError):
        return jsonify({"error": "side, slot, player_id required"}), 400
    if side not in ("A", "B") or slot not in (1, 2):
        return jsonify({"error": "invalid side or slot"}), 400
    p = db.get_player(player_id)
    if not p:
        return jsonify({"error": "player not found"}), 404
    if not db.get_table(table_id):
        abort(404)

    db.add_live_player(table_id, side, slot, player_id, p["rfid_uid"], None)
    lt = db.get_live_table(table_id)
    team_a = [r for r in db.get_live_players(table_id) if r["team_side"] == "A"]
    team_b = [r for r in db.get_live_players(table_id) if r["team_side"] == "B"]
    has_a1 = any(r["slot"] == 1 for r in team_a)
    has_b1 = any(r["slot"] == 1 for r in team_b)
    mode = mqtt_client._compute_mode(team_a, team_b)  # noqa: SLF001
    if lt["state"] != "GAME_PLAYING" and has_a1 and has_b1:
        db.update_live_table(table_id, state="GAME_PLAYING", mode=mode,
                             started_at=db.utc_now())
    else:
        new_state = "PLAYERS_REGISTERING" if lt["state"] != "GAME_PLAYING" else "GAME_PLAYING"
        db.update_live_table(table_id, state=new_state, mode=mode)
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/table/<int:table_id>/remove_player", methods=["POST"])
@admin_required
def api_admin_remove_player(table_id: int):
    data = request.get_json(silent=True) or {}
    side = data.get("side")
    try:
        slot = int(data["slot"])
    except (KeyError, TypeError, ValueError):
        return jsonify({"error": "slot required"}), 400
    if side not in ("A", "B") or slot not in (1, 2):
        return jsonify({"error": "invalid side or slot"}), 400

    db.remove_live_player(table_id, side, slot)
    db.compact_slots(table_id, side)
    lt = db.get_live_table(table_id)
    if lt["state"] == "GAME_PLAYING" and db.count_side(table_id, side) == 0:
        mqtt_client._abandon_match(table_id, reason="admin remove")  # noqa: SLF001
    _refresh_table(table_id)
    return jsonify({"ok": True})


@app.route("/api/admin/player/<int:player_id>/rename", methods=["POST"])
@admin_required
def api_admin_rename_player(player_id: int):
    name = ((request.get_json(silent=True) or {}).get("name") or "").strip()
    if not name:
        return jsonify({"error": "name required"}), 400
    p = db.get_player(player_id)
    if not p:
        abort(404)
    if db.name_collides(name, excluding_id=player_id):
        return jsonify({"error": f"another player already named {name!r}"}), 400
    db.rename_player(player_id, name, register=True)
    state.broadcast("player_update", {"players": db.list_players_with_stats()})
    return jsonify({"ok": True})


# ---------------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------------

def _refresh_table(table_id: int) -> None:
    snap = db.table_snapshot(table_id)
    if not snap:
        return
    state.set_table_snapshot(table_id, snap)
    state.broadcast("table_state", snap)
    mqtt_client.publish_sync(table_id, snap)


def _heartbeat_watchdog() -> None:
    """Mark Picos offline if they've been silent for too long."""
    while True:
        try:
            time.sleep(5)
            cutoff = (datetime.now(timezone.utc) - timedelta(seconds=PICO_OFFLINE_SEC))
            cutoff_iso = cutoff.strftime("%Y-%m-%d %H:%M:%S")
            stale = db.stale_picos(cutoff_iso)
            for pico_id in stale:
                db.set_pico_online(pico_id, False)
                state.set_pico_status(pico_id, online=False)
                state.broadcast("pico_status", {"pico_id": pico_id, "online": False})
                log.warning("Pico %s marked offline (no heartbeat in %ds)",
                            pico_id, PICO_OFFLINE_SEC)
        except Exception:  # noqa: BLE001
            log.exception("watchdog tick failed")


def _bootstrap() -> None:
    log.info("Initializing database at %s", db.DB_PATH if hasattr(db, "DB_PATH") else "(default)")
    db.init_db()
    db.startup_reset()
    # Prime the in-memory cache with whatever snapshots survived restart.
    for snap in db.all_table_snapshots():
        state.set_table_snapshot(snap["table_id"], snap)

    mqtt_client.start()
    threading.Thread(target=_heartbeat_watchdog, name="watchdog", daemon=True).start()
    log.info("Server bootstrap complete")


_bootstrap()


if __name__ == "__main__":
    # threaded=True is required: Flask's dev server would otherwise serialize
    # SSE connections behind one another.
    app.run(host="0.0.0.0", port=5000, threaded=True, debug=False)
