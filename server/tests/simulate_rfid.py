"""Publish a single RFID tap.

Usage:
    python tests/simulate_rfid.py UID [side] [table_id] [pico_id]

Examples:
    python tests/simulate_rfid.py DBEF7005 A
    python tests/simulate_rfid.py 4C069804 B 1 sim-B
"""

from __future__ import annotations

import json
import sys
import time

import paho.mqtt.client as mqtt


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    uid = sys.argv[1].upper()
    side = sys.argv[2] if len(sys.argv) > 2 else "A"
    table_id = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    pico_id = sys.argv[4] if len(sys.argv) > 4 else f"sim-{side}"

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"sim-rfid-{uid}",
    )
    client.connect("localhost", 1883, keepalive=60)
    client.loop_start()

    payload = {
        "v": 1,
        "pico_id": pico_id,
        "table_id": table_id,
        "side": side,
        "slot": 1,
        "uid": uid,
        "event": "card_tapped",
        "ts": int(time.time() * 1000),
    }
    info = client.publish(
        f"tablefootball/table/{table_id}/rfid",
        json.dumps(payload),
        qos=1,
    )
    info.wait_for_publish(timeout=2)
    print(f"Sent RFID tap: table={table_id} side={side} uid={uid} pico={pico_id}")
    time.sleep(0.5)
    client.loop_stop()
    client.disconnect()


if __name__ == "__main__":
    main()
