"""MQTT bridge: Pico ↔ server.

Subscribes to `tablefootball/#`, routes messages by topic, updates the
database via `database.py`, refreshes the in-memory snapshot in
`state.py`, and pushes SSE events to browsers.

Publish helpers send player/sync/cmd messages back to the Picos.
"""

from __future__ import annotations

import json
import logging
import threading
import time
from typing import Any

import paho.mqtt.client as mqtt

import database as db
import state

from config import MQTT_HOST, MQTT_PORT

log = logging.getLogger(__name__)

_client: mqtt.Client | None = None
_connected = threading.Event()

# In-memory dedup: (uid, table_id) -> monotonic-ms of last accepted tap.
_RFID_DEDUP_WINDOW_MS = 500
_recent_taps: dict[tuple[str, int], float] = {}
_recent_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _normalize_uid(uid: str | None) -> str | None:
    if not uid:
        return None
    return uid.strip().upper().replace(":", "").replace(" ", "")


def _compute_mode(team_a: list, team_b: list) -> str | None:
    a, b = len(team_a), len(team_b)
    if a == 1 and b == 1:
        return "1v1"
    if a == 2 and b == 2:
        return "2v2"
    if (a == 1 and b == 2) or (a == 2 and b == 1):
        return "mixed"
    return None


def _refresh_snapshot_and_broadcast(table_id: int) -> None:
    """Pull the latest DB snapshot, store in state, push SSE + MQTT sync."""
    snap = db.table_snapshot(table_id)
    if not snap:
        return
    state.set_table_snapshot(table_id, snap)
    state.broadcast("table_state", snap)
    publish_sync(table_id, snap)


def _maybe_dedup(uid: str, table_id: int) -> bool:
    """Return True if this tap should be DROPPED (duplicate within window)."""
    now = time.monotonic() * 1000
    key = (uid, table_id)
    with _recent_lock:
        last = _recent_taps.get(key)
        if last is not None and (now - last) < _RFID_DEDUP_WINDOW_MS:
            return True
        _recent_taps[key] = now
        # Light periodic cleanup so the dict doesn't grow forever.
        if len(_recent_taps) > 256:
            cutoff = now - 5000
            for k, v in list(_recent_taps.items()):
                if v < cutoff:
                    _recent_taps.pop(k, None)
    return False


# ---------------------------------------------------------------------------
# Handlers
# ---------------------------------------------------------------------------

def _handle_heartbeat(payload: dict) -> None:
    pico_id = payload.get("pico_id")
    if not pico_id:
        return
    table_id = payload.get("table_id")
    db.upsert_pico_from_heartbeat(
        pico_id=pico_id,
        table_id=int(table_id) if table_id is not None else None,
        side=payload.get("side"),
        role=payload.get("role"),
        ip=payload.get("ip"),
        firmware=payload.get("firmware"),
    )
    state.set_pico_status(pico_id, online=True, last_seen=db.utc_now())
    state.broadcast("pico_status", {"pico_id": pico_id, "online": True})
    publish_players_list(pico_id)


def _handle_status_lwt(pico_id: str, payload: dict) -> None:
    """The LWT topic carries `online:false` when a Pico drops."""
    if payload.get("online") is False:
        db.set_pico_online(pico_id, False)
        state.set_pico_status(pico_id, online=False)
        state.broadcast("pico_status", {"pico_id": pico_id, "online": False})


def _handle_rfid(table_id: int, payload: dict) -> None:
    uid = _normalize_uid(payload.get("uid"))
    pico_id = payload.get("pico_id")
    side = payload.get("side")
    if not uid or side not in ("A", "B"):
        log.warning("Invalid RFID payload on table %s: %s", table_id, payload)
        return

    if _maybe_dedup(uid, table_id):
        log.debug("Dropped duplicate RFID %s on table %s", uid, table_id)
        return

    lt = db.get_live_table(table_id)
    if not lt:
        log.warning("RFID for unknown table %s", table_id)
        return

    player = db.get_or_create_player_by_uid(uid)

    existing = db.find_live_player_by_uid(table_id, uid)
    if existing:
        _deregister(table_id, existing, payload, pico_id, player)
    else:
        _register(table_id, side, uid, pico_id, player, lt)

    _refresh_snapshot_and_broadcast(table_id)
    _send_player_response(pico_id, uid, player, table_id)


def _register(table_id: int, side: str, uid: str, pico_id: str | None,
              player, lt) -> None:
    slot = db.first_empty_slot(table_id, side)
    if slot is None:
        db.log_rfid_event(table_id, pico_id, side, None, uid, player["id"], "ignored")
        log.info("Ignored RFID %s — side %s full on table %s", uid, side, table_id)
        return

    db.add_live_player(table_id, side, slot, player["id"], uid, pico_id)
    db.log_rfid_event(table_id, pico_id, side, slot, uid, player["id"], "register")

    cur_state = lt["state"]
    team_a = [r for r in db.get_live_players(table_id) if r["team_side"] == "A"]
    team_b = [r for r in db.get_live_players(table_id) if r["team_side"] == "B"]
    mode = _compute_mode(team_a, team_b)

    has_a1 = any(r["slot"] == 1 for r in team_a)
    has_b1 = any(r["slot"] == 1 for r in team_b)

    if cur_state == "GAME_PLAYING":
        # Mid-game join — no state change.
        db.update_live_table(table_id, mode=mode)
        return

    if has_a1 and has_b1:
        db.update_live_table(
            table_id,
            state="GAME_PLAYING",
            mode=mode,
            score_a=0,
            score_b=0,
            fastest_shot=0,
            winner_side=None,
            winner_player_id=None,
            started_at=db.utc_now(),
        )
        log.info("Table %s auto-started (mode=%s)", table_id, mode)
    else:
        db.update_live_table(table_id, state="PLAYERS_REGISTERING", mode=mode)


def _deregister(table_id: int, existing, payload: dict,
                pico_id: str | None, player) -> None:
    side = existing["team_side"]
    slot = existing["slot"]
    db.remove_live_player(table_id, side, slot)
    db.compact_slots(table_id, side)
    db.log_rfid_event(table_id, pico_id, side, slot, existing["rfid_uid"],
                      player["id"], "deregister")

    lt = db.get_live_table(table_id)
    cur_state = lt["state"] if lt else "WAITING"

    side_count = db.count_side(table_id, side)
    team_a = [r for r in db.get_live_players(table_id) if r["team_side"] == "A"]
    team_b = [r for r in db.get_live_players(table_id) if r["team_side"] == "B"]

    if cur_state == "GAME_PLAYING" and side_count == 0:
        _abandon_match(table_id, reason="side emptied")
        return

    mode = _compute_mode(team_a, team_b)
    if cur_state == "GAME_PLAYING":
        db.update_live_table(table_id, mode=mode)
        return

    # Registering / waiting branches.
    if not team_a and not team_b:
        db.update_live_table(table_id, state="WAITING", mode=None)
    else:
        db.update_live_table(table_id, state="PLAYERS_REGISTERING", mode=mode)


def _abandon_match(table_id: int, reason: str) -> None:
    """Save partial match (result_type='abandoned') and reset to WAITING."""
    lt = db.get_live_table(table_id)
    if not lt:
        return
    players = []
    for lp in db.get_live_players(table_id):
        if lp["player_id"]:
            players.append({
                "player_id": lp["player_id"],
                "team_side": lp["team_side"],
                "slot": lp["slot"],
            })
    if players:
        db.save_match(
            table_id=table_id,
            mode=lt["mode"] or "1v1",
            score_a=lt["score_a"],
            score_b=lt["score_b"],
            winner_side=None,
            winner_player_id=None,
            fastest_shot=lt["fastest_shot"],
            result_type="abandoned",
            started_at=lt["started_at"],
            session_id=lt["session_id"],
            players=players,
        )
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
    log.info("Table %s abandoned (%s)", table_id, reason)


def _handle_state(table_id: int, payload: dict) -> None:
    """Primary Pico publishes score/fastest/GAME_OVER here."""
    lt = db.get_live_table(table_id)
    if not lt:
        return

    score_a = int(payload.get("score_a", lt["score_a"]))
    score_b = int(payload.get("score_b", lt["score_b"]))
    fastest = float(payload.get("fastest", lt["fastest_shot"] or 0.0))
    session_id = payload.get("session_id") or lt["session_id"]
    pico_reported = payload.get("state")

    updates: dict[str, Any] = {
        "score_a": score_a,
        "score_b": score_b,
        "fastest_shot": fastest,
        "session_id": session_id,
    }
    if lt["state"] in ("PLAYERS_REGISTERING", "WAITING") and pico_reported == "GAME_PLAYING":
        # Server normally drives the transition, but if the Pico starts a game
        # while both slot 1s are filled we honor it.
        team_a = [r for r in db.get_live_players(table_id) if r["team_side"] == "A"]
        team_b = [r for r in db.get_live_players(table_id) if r["team_side"] == "B"]
        if any(r["slot"] == 1 for r in team_a) and any(r["slot"] == 1 for r in team_b):
            updates["state"] = "GAME_PLAYING"
            updates["mode"] = _compute_mode(team_a, team_b)
            if not lt["started_at"]:
                updates["started_at"] = db.utc_now()

    if pico_reported == "GAME_OVER" and lt["state"] != "GAME_OVER":
        _finish_match(table_id, score_a, score_b, fastest, session_id, payload)
        return  # _finish_match already refreshes

    db.update_live_table(table_id, **updates)
    _refresh_snapshot_and_broadcast(table_id)


def _finish_match(table_id: int, score_a: int, score_b: int, fastest: float,
                  session_id: str | None, payload: dict) -> None:
    lt = db.get_live_table(table_id)
    winner_side = "A" if score_a > score_b else ("B" if score_b > score_a else None)

    winner_uid = _normalize_uid(payload.get("winner_uid"))
    winner_player = db.get_player_by_uid(winner_uid) if winner_uid else None

    live_players = list(db.get_live_players(table_id))
    players = [
        {"player_id": r["player_id"], "team_side": r["team_side"], "slot": r["slot"]}
        for r in live_players if r["player_id"]
    ]

    db.save_match(
        table_id=table_id,
        mode=lt["mode"] or "1v1",
        score_a=score_a,
        score_b=score_b,
        winner_side=winner_side,
        winner_player_id=winner_player["id"] if winner_player else None,
        fastest_shot=fastest,
        result_type="completed",
        started_at=lt["started_at"],
        session_id=session_id,
        players=players,
    )
    db.update_live_table(
        table_id,
        state="GAME_OVER",
        score_a=score_a,
        score_b=score_b,
        fastest_shot=fastest,
        winner_side=winner_side,
        winner_player_id=winner_player["id"] if winner_player else None,
        session_id=session_id,
    )
    log.info("Table %s GAME_OVER %s-%s, winner=%s", table_id, score_a, score_b, winner_side)
    _refresh_snapshot_and_broadcast(table_id)


def _handle_ball(table_id: int, payload: dict) -> None:
    snap = state.get_table_snapshot(table_id)
    if not snap or snap.get("state") != "GAME_PLAYING":
        return
    state.broadcast(
        "ball_position",
        {
            "table_id": table_id,
            "x": payload.get("x", 0),
            "y": payload.get("y", 0),
            "speed": payload.get("speed", 0),
        },
        lossy=True,
    )


# ---------------------------------------------------------------------------
# Connection
# ---------------------------------------------------------------------------

def _on_connect(client, _userdata, _flags, rc, _properties=None) -> None:
    if rc == 0:
        log.info("MQTT connected to %s:%s", MQTT_HOST, MQTT_PORT)
        client.subscribe("tablefootball/#", qos=1)
        _connected.set()
    else:
        log.error("MQTT connect failed (rc=%s)", rc)


def _on_disconnect(_client, _userdata, rc, _properties=None) -> None:
    log.warning("MQTT disconnected (rc=%s)", rc)
    _connected.clear()


def _on_message(_client, _userdata, msg) -> None:
    try:
        payload = json.loads(msg.payload.decode("utf-8")) if msg.payload else {}
    except (UnicodeDecodeError, json.JSONDecodeError):
        log.warning("Non-JSON MQTT payload on %s", msg.topic)
        return

    log.debug("MQTT %s %s", msg.topic, payload)
    parts = msg.topic.split("/")
    try:
        # tablefootball/pico/<id>/{heartbeat,status,...}
        if len(parts) == 4 and parts[0] == "tablefootball" and parts[1] == "pico":
            pico_id, leaf = parts[2], parts[3]
            if leaf == "heartbeat":
                _handle_heartbeat(payload)
            elif leaf == "status":
                _handle_status_lwt(pico_id, payload)
            return
        # tablefootball/table/<id>/{rfid,state,ball}
        if len(parts) == 4 and parts[0] == "tablefootball" and parts[1] == "table":
            try:
                table_id = int(parts[2])
            except ValueError:
                return
            leaf = parts[3]
            if leaf == "rfid":
                _handle_rfid(table_id, payload)
            elif leaf == "state":
                _handle_state(table_id, payload)
            elif leaf == "ball":
                _handle_ball(table_id, payload)
    except Exception:  # noqa: BLE001
        log.exception("Error handling MQTT message on %s", msg.topic)


def start() -> None:
    """Start MQTT client in a background thread with retry/backoff."""
    global _client
    _client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="football-server",
        clean_session=True,
    )
    _client.on_connect = _on_connect
    _client.on_disconnect = _on_disconnect
    _client.on_message = _on_message
    # paho's built-in reconnect: 1s → 2s → ... up to 30s.
    _client.reconnect_delay_set(min_delay=1, max_delay=30)

    def _connect_loop():
        backoff = 1
        while True:
            try:
                _client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
                _client.loop_forever(retry_first_connection=False)
            except Exception as e:  # noqa: BLE001
                log.warning("MQTT connect retry in %ds (%s)", backoff, e)
                time.sleep(backoff)
                backoff = min(backoff * 2, 30)

    threading.Thread(target=_connect_loop, name="mqtt", daemon=True).start()


# ---------------------------------------------------------------------------
# Publishers
# ---------------------------------------------------------------------------

def _pub(topic: str, payload: dict, qos: int = 1, retain: bool = False) -> None:
    if _client is None:
        log.warning("Tried to publish before MQTT client started")
        return
    body = {"v": 1, "ts": int(time.time() * 1000), **payload}
    _client.publish(topic, json.dumps(body, separators=(",", ":")),
                    qos=qos, retain=retain)


def publish_sync(table_id: int, snap: dict | None = None) -> None:
    snap = snap or db.table_snapshot(table_id)
    if not snap:
        return
    _pub(
        f"tablefootball/table/{table_id}/sync",
        {
            "table_id": table_id,
            "state": snap["state"],
            "mode": snap.get("mode"),
            "score_a": snap["score_a"],
            "score_b": snap["score_b"],
            "max_score": snap["max_score"],
            "team_a": snap["team_a"],
            "team_b": snap["team_b"],
        },
        qos=1,
        retain=True,
    )


def _send_player_response(pico_id: str | None, uid: str, player, table_id: int) -> None:
    if not pico_id:
        return
    lp = db.find_live_player_by_uid(table_id, uid)
    if not lp:
        # Tap was a deregister; tell the Pico the player is no longer seated.
        _pub(
            f"tablefootball/pico/{pico_id}/player",
            {
                "uid": uid,
                "player_id": player["id"],
                "name": player["name"],
                "is_guest": bool(player["is_guest"]),
                "side": None,
                "slot": None,
                "seated": False,
            },
            qos=1,
        )
        return
    _pub(
        f"tablefootball/pico/{pico_id}/player",
        {
            "uid": uid,
            "player_id": player["id"],
            "name": player["name"],
            "is_guest": bool(player["is_guest"]),
            "side": lp["team_side"],
            "slot": lp["slot"],
            "seated": True,
        },
        qos=1,
    )


def publish_players_list(pico_id: str) -> None:
    _pub(
        f"tablefootball/pico/{pico_id}/players_list",
        {"players": db.registered_players_list()},
        qos=1,
        retain=True,
    )


def publish_cmd(pico_id: str, payload: dict) -> None:
    _pub(f"tablefootball/pico/{pico_id}/cmd", payload, qos=1)
