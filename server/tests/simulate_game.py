"""Run a complete simulated match against the local server.

What this does:
  1. Sends heartbeats for two fake Picos (primary on side A, secondary on B).
  2. Taps two RFID UIDs (one per side) to start a game — unless --no-register
     is passed, e.g. when the two players were already seated some other way
     (a tournament bracket match assigned to a table seats its two entries
     directly in the database, no physical tap involved).
  3. Streams state messages while incrementing scores until someone wins.
  4. Sends a few ball_position frames during play so SSE has something to draw.
  5. Sends the GAME_OVER state and exits.

Usage:
    python tests/simulate_game.py
    python tests/simulate_game.py --uid-a DB4E6C05 --uid-b C2878704 --no-register
    python tests/simulate_game.py --table-id 1 --max-score 3 --winner b
"""

from __future__ import annotations

import argparse
import json
import random
import time
from datetime import datetime

import paho.mqtt.client as mqtt

HOST = "localhost"
PORT = 1883


def make_client(cid: str) -> mqtt.Client:
    c = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    c.connect(HOST, PORT, keepalive=60)
    c.loop_start()
    return c


def hb(c: mqtt.Client, table_id: int, pico_id: str, side: str, role: str) -> None:
    payload = {
        "v": 1, "pico_id": pico_id, "table_id": table_id, "side": side, "role": role,
        "ip": "127.0.0.1", "firmware": "sim-0.1.0", "uptime": 1,
        "ts": int(time.time() * 1000),
    }
    c.publish(f"tablefootball/pico/{pico_id}/heartbeat", json.dumps(payload), qos=0)


def rfid_tap(c: mqtt.Client, table_id: int, uid: str, side: str, pico_id: str) -> None:
    payload = {
        "v": 1, "pico_id": pico_id, "table_id": table_id, "side": side, "slot": 1,
        "uid": uid, "event": "card_tapped", "ts": int(time.time() * 1000),
    }
    c.publish(f"tablefootball/table/{table_id}/rfid", json.dumps(payload), qos=1)


def state(c: mqtt.Client, table_id: int, pico_a: str, s: str, score_a: int, score_b: int,
          fastest: float, session_id: str,
          winner_side: str = "", winner_uid: str = "") -> None:
    payload = {
        "v": 1, "pico_id": pico_a, "table_id": table_id, "session_id": session_id,
        "state": s, "mode": "1v1",
        "score_a": score_a, "score_b": score_b,
        "fastest": fastest, "winner_side": winner_side, "winner_uid": winner_uid,
        "time": 0, "ts": int(time.time() * 1000),
    }
    c.publish(f"tablefootball/table/{table_id}/state", json.dumps(payload), qos=1)


def ball(c: mqtt.Client, table_id: int, x: float, y: float, speed: float) -> None:
    payload = {"v": 1, "table_id": table_id, "x": x, "y": y, "speed": speed,
               "ts": int(time.time() * 1000)}
    c.publish(f"tablefootball/table/{table_id}/ball", json.dumps(payload), qos=0)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--table-id", type=int, default=1)
    p.add_argument("--max-score", type=int, default=5,
                   help="goals to win (default: 5, short so the demo finishes quickly)")
    p.add_argument("--uid-a", default="DBEF7005", help="Side A player's RFID UID (default: Alice)")
    p.add_argument("--uid-b", default="4C069804", help="Side B player's RFID UID (default: Bob)")
    p.add_argument("--pico-a", default="sim-A", help="fake pico_id to publish as for side A")
    p.add_argument("--pico-b", default="sim-B", help="fake pico_id to publish as for side B")
    p.add_argument("--winner", choices=["a", "b", "random"], default="random",
                   help="force which side wins instead of leaving it to random scoring")
    p.add_argument("--no-register", action="store_true",
                   help="skip the RFID-tap registration step — use when the two "
                        "players are already seated some other way (e.g. a "
                        "tournament bracket match just got assigned to this "
                        "table), since tapping here would re-register/interfere "
                        "with that existing roster instead of just reporting a result")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    table_id = args.table_id

    a = make_client("sim-a-client")
    b = make_client("sim-b-client")
    print("Sending heartbeats...")
    for _ in range(2):
        hb(a, table_id, args.pico_a, "A", "primary")
        hb(b, table_id, args.pico_b, "B", "secondary")
        time.sleep(0.5)

    if args.no_register:
        print("Skipping registration (--no-register) — assuming both players "
              "are already seated on this table.")
    else:
        print("Registering players...")
        rfid_tap(a, table_id, args.uid_a, "A", args.pico_a)
        time.sleep(0.4)
        rfid_tap(b, table_id, args.uid_b, "B", args.pico_b)
        time.sleep(0.6)

    session_id = "sim-" + datetime.utcnow().strftime("%Y%m%d-%H%M%S")
    score_a = score_b = 0
    fastest = 0.0
    print(f"Playing match (session={session_id}, first to {args.max_score})...")
    bx, by = 500.0, 250.0
    while score_a < args.max_score and score_b < args.max_score:
        # Move ball around a few frames.
        for _ in range(6):
            bx = max(20.0, min(980.0, bx + random.uniform(-60, 60)))
            by = max(20.0, min(480.0, by + random.uniform(-30, 30)))
            ball(a, table_id, bx, by, random.uniform(2.0, 18.0))
            time.sleep(0.1)
        if random.random() < 0.5:
            score_a += 1
        else:
            score_b += 1
        fastest = max(fastest, round(random.uniform(8.0, 18.5), 1))
        print(f"  score: {score_a}-{score_b}  fastest: {fastest}")
        state(a, table_id, args.pico_a, "GAME_PLAYING", score_a, score_b, fastest, session_id)
        time.sleep(0.5)

    if args.winner == "random":
        winner_side = "A" if score_a > score_b else "B"
    else:
        winner_side = args.winner.upper()
        # Make the reported score agree with the forced winner so the server's
        # own score_a > score_b check doesn't disagree with winner_side.
        if winner_side == "A" and score_a <= score_b:
            score_a, score_b = args.max_score, score_b
        elif winner_side == "B" and score_b <= score_a:
            score_a, score_b = score_a, args.max_score

    winner_uid = args.uid_a if winner_side == "A" else args.uid_b
    print(f"GAME_OVER winner side {winner_side} (uid={winner_uid})")
    state(a, table_id, args.pico_a, "GAME_OVER", score_a, score_b, fastest, session_id,
          winner_side=winner_side, winner_uid=winner_uid)
    time.sleep(0.5)

    for c in (a, b):
        c.loop_stop()
        c.disconnect()


if __name__ == "__main__":
    main()
