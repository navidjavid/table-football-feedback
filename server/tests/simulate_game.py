"""Run a complete simulated match against the local server.

What this does:
  1. Sends heartbeats for two fake Picos (primary on side A, secondary on B).
  2. Taps two RFID UIDs (one per side) to start a game.
  3. Streams state messages while incrementing scores until someone wins.
  4. Sends a few ball_position frames during play so SSE has something to draw.
  5. Sends the GAME_OVER state and exits.

Usage:
    python tests/simulate_game.py
"""

from __future__ import annotations

import json
import random
import time
from datetime import datetime

import paho.mqtt.client as mqtt

HOST = "localhost"
PORT = 1883
TABLE_ID = 1
MAX_SCORE = 5  # short so the demo finishes quickly
UID_A = "DBEF7005"
UID_B = "4C069804"
PICO_A = "sim-A"
PICO_B = "sim-B"


def make_client(cid: str) -> mqtt.Client:
    c = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    c.connect(HOST, PORT, keepalive=60)
    c.loop_start()
    return c


def hb(c: mqtt.Client, pico_id: str, side: str, role: str) -> None:
    payload = {
        "v": 1, "pico_id": pico_id, "table_id": TABLE_ID, "side": side, "role": role,
        "ip": "127.0.0.1", "firmware": "sim-0.1.0", "uptime": 1,
        "ts": int(time.time() * 1000),
    }
    c.publish(f"tablefootball/pico/{pico_id}/heartbeat", json.dumps(payload), qos=0)


def rfid_tap(c: mqtt.Client, uid: str, side: str, pico_id: str) -> None:
    payload = {
        "v": 1, "pico_id": pico_id, "table_id": TABLE_ID, "side": side, "slot": 1,
        "uid": uid, "event": "card_tapped", "ts": int(time.time() * 1000),
    }
    c.publish(f"tablefootball/table/{TABLE_ID}/rfid", json.dumps(payload), qos=1)


def state(c: mqtt.Client, s: str, score_a: int, score_b: int,
          fastest: float, session_id: str,
          winner_side: str = "", winner_uid: str = "") -> None:
    payload = {
        "v": 1, "pico_id": PICO_A, "table_id": TABLE_ID, "session_id": session_id,
        "state": s, "mode": "1v1",
        "score_a": score_a, "score_b": score_b,
        "fastest": fastest, "winner_side": winner_side, "winner_uid": winner_uid,
        "time": 0, "ts": int(time.time() * 1000),
    }
    c.publish(f"tablefootball/table/{TABLE_ID}/state", json.dumps(payload), qos=1)


def ball(c: mqtt.Client, x: float, y: float, speed: float) -> None:
    payload = {"v": 1, "table_id": TABLE_ID, "x": x, "y": y, "speed": speed,
               "ts": int(time.time() * 1000)}
    c.publish(f"tablefootball/table/{TABLE_ID}/ball", json.dumps(payload), qos=0)


def main() -> None:
    a = make_client("sim-a-client")
    b = make_client("sim-b-client")
    print("Sending heartbeats...")
    for _ in range(2):
        hb(a, PICO_A, "A", "primary")
        hb(b, PICO_B, "B", "secondary")
        time.sleep(0.5)

    print("Registering players...")
    rfid_tap(a, UID_A, "A", PICO_A)
    time.sleep(0.4)
    rfid_tap(b, UID_B, "B", PICO_B)
    time.sleep(0.6)

    session_id = "sim-" + datetime.utcnow().strftime("%Y%m%d-%H%M%S")
    score_a = score_b = 0
    fastest = 0.0
    print(f"Playing match (session={session_id}, first to {MAX_SCORE})...")
    bx, by = 500.0, 250.0
    while score_a < MAX_SCORE and score_b < MAX_SCORE:
        # Move ball around a few frames.
        for _ in range(6):
            bx = max(20.0, min(980.0, bx + random.uniform(-60, 60)))
            by = max(20.0, min(480.0, by + random.uniform(-30, 30)))
            ball(a, bx, by, random.uniform(2.0, 18.0))
            time.sleep(0.1)
        if random.random() < 0.5:
            score_a += 1
        else:
            score_b += 1
        fastest = max(fastest, round(random.uniform(8.0, 18.5), 1))
        print(f"  score: {score_a}-{score_b}  fastest: {fastest}")
        state(a, "GAME_PLAYING", score_a, score_b, fastest, session_id)
        time.sleep(0.5)

    winner_side = "A" if score_a > score_b else "B"
    winner_uid = UID_A if winner_side == "A" else UID_B
    print(f"GAME_OVER winner side {winner_side}")
    state(a, "GAME_OVER", score_a, score_b, fastest, session_id,
          winner_side=winner_side, winner_uid=winner_uid)
    time.sleep(0.5)

    for c in (a, b):
        c.loop_stop()
        c.disconnect()


if __name__ == "__main__":
    main()
