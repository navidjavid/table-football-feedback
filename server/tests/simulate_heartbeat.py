"""Publish a heartbeat from a fake Pico every 5 seconds.

Usage:
    python tests/simulate_heartbeat.py [pico_id] [table_id] [side] [role]

Defaults: pico_id="sim-A", table_id=1, side="A", role="primary".
"""

from __future__ import annotations

import json
import sys
import time
import socket

import paho.mqtt.client as mqtt


def main() -> None:
    pico_id = sys.argv[1] if len(sys.argv) > 1 else "sim-A"
    table_id = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    side = sys.argv[3] if len(sys.argv) > 3 else "A"
    role = sys.argv[4] if len(sys.argv) > 4 else "primary"

    host = "localhost"
    port = 1883

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"sim-{pico_id}",
    )
    # Configure last-will so the server marks us offline if we crash.
    client.will_set(
        f"tablefootball/pico/{pico_id}/status",
        json.dumps({"v": 1, "pico_id": pico_id, "online": False, "reason": "lwt"}),
        qos=1,
        retain=True,
    )
    client.connect(host, port, keepalive=60)
    client.loop_start()

    ip = socket.gethostbyname(socket.gethostname())
    start = time.monotonic()
    print(f"Heartbeating as {pico_id} (table {table_id} side {side} role {role})... Ctrl-C to stop")
    try:
        while True:
            payload = {
                "v": 1,
                "pico_id": pico_id,
                "table_id": table_id,
                "side": side,
                "role": role,
                "ip": ip,
                "firmware": "sim-0.1.0",
                "uptime": int(time.monotonic() - start),
                "ts": int(time.time() * 1000),
            }
            client.publish(
                f"tablefootball/pico/{pico_id}/heartbeat",
                json.dumps(payload),
                qos=0,
            )
            time.sleep(5)
    except KeyboardInterrupt:
        # Tell the server we're going away cleanly.
        client.publish(
            f"tablefootball/pico/{pico_id}/status",
            json.dumps({"v": 1, "pico_id": pico_id, "online": False, "reason": "shutdown"}),
            qos=1, retain=True,
        )
        time.sleep(0.2)
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
