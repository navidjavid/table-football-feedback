"""In-memory live state + Server-Sent-Events fan-out.

Two reasons to keep state in memory in addition to the DB:
  1. Reading the latest snapshot is fast (no SQL round-trip per SSE push).
  2. Ball position is high-frequency and never written to the DB — it
     lives only here on its way through to browser clients.

Thread safety: every read or write of the dicts/queues here goes through
`_lock`. Callers (MQTT handlers, Flask routes) do not need to lock.
"""

from __future__ import annotations

import json
import logging
import queue
import threading
from typing import Any

log = logging.getLogger(__name__)

_lock = threading.Lock()

# table_id -> latest snapshot dict (mirrors database.table_snapshot output)
_tables: dict[int, dict] = {}

# pico_id -> {"online": bool, "last_seen": iso_str, ...}
_picos: dict[str, dict] = {}

# Connected SSE clients. Each client owns a bounded Queue.
_clients: list[queue.Queue] = []

# Cap per-client queue. Ball positions arrive at ~10Hz and are dropped
# rather than blocking other events when a slow client falls behind.
_QUEUE_MAX = 20


# ---------------------------------------------------------------------------
# Table state
# ---------------------------------------------------------------------------

def set_table_snapshot(table_id: int, snapshot: dict) -> None:
    with _lock:
        _tables[int(table_id)] = dict(snapshot)


def get_table_snapshot(table_id: int) -> dict | None:
    with _lock:
        s = _tables.get(int(table_id))
        return dict(s) if s else None


def all_snapshots() -> list[dict]:
    with _lock:
        return [dict(s) for s in _tables.values()]


# ---------------------------------------------------------------------------
# Pico status
# ---------------------------------------------------------------------------

def set_pico_status(pico_id: str, online: bool, last_seen: str | None = None) -> None:
    with _lock:
        prev = _picos.get(pico_id, {})
        prev["online"] = online
        if last_seen is not None:
            prev["last_seen"] = last_seen
        _picos[pico_id] = prev


def get_pico_status(pico_id: str) -> dict:
    with _lock:
        return dict(_picos.get(pico_id, {"online": False}))


def list_pico_statuses() -> dict[str, dict]:
    with _lock:
        return {k: dict(v) for k, v in _picos.items()}


# ---------------------------------------------------------------------------
# SSE fan-out
# ---------------------------------------------------------------------------

def register_sse_client() -> queue.Queue:
    """Allocate a queue for a freshly-connected SSE client."""
    q: queue.Queue = queue.Queue(maxsize=_QUEUE_MAX)
    with _lock:
        _clients.append(q)
        n = len(_clients)
    log.debug("SSE client connected (%d total)", n)
    return q


def unregister_sse_client(q: queue.Queue) -> None:
    with _lock:
        try:
            _clients.remove(q)
        except ValueError:
            pass
        n = len(_clients)
    log.debug("SSE client disconnected (%d remaining)", n)


def _format_event(event: str, data: Any) -> str:
    payload = json.dumps(data, separators=(",", ":"))
    return f"event: {event}\ndata: {payload}\n\n"


def broadcast(event: str, data: Any, lossy: bool = False) -> None:
    """Push an SSE event to every connected client.

    If `lossy=True`, drop the event for any client whose queue is full
    rather than blocking. Use this for ball_position (10Hz).
    """
    msg = _format_event(event, data)
    with _lock:
        clients = list(_clients)
    for q in clients:
        if lossy:
            try:
                q.put_nowait(msg)
            except queue.Full:
                pass
        else:
            try:
                q.put(msg, timeout=1.0)
            except queue.Full:
                log.warning("SSE client queue full — dropping non-lossy event %s", event)
