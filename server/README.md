# Table Football — Raspberry Pi Server

The Raspberry Pi 4 acts as the central hub for the table football
project. It runs as a WiFi hotspot, MQTT broker, Flask web server, and
SQLite database, all starting automatically on boot.

```
+----------------+        +-------------+         +-------------+
| Pico Side A    |  MQTT  |             |  SSE    | Phone /     |
| (RFID, LCD)    | <----> |   Pi 4      | ------> | Browser     |
+----------------+        |             |         +-------------+
                          | mosquitto   |
+----------------+        | flask app   |
| Pico Side B    |  MQTT  | sqlite (WAL)|
| (RFID, LCD)    | <----> |             |
+----------------+        +-------------+

Hotspot: SSID "TableFootball"  -  192.168.4.1
Browser: http://192.168.4.1:5000
MQTT:    192.168.4.1:1883
```

## What this folder contains

```
server/
├── app.py              Flask app + HTTP routes + SSE + startup wiring
├── mqtt_client.py      MQTT subscribe/publish + game-state handlers
├── database.py         All SQLite access (thread-local connections, WAL)
├── state.py            In-memory live snapshots + SSE fan-out
├── config.py           Loads .env, configures rotating-file logging
├── requirements.txt    Python deps
├── .env.example        Copy to .env and fill in
├── templates/          Jinja templates (dashboard, player, admin)
├── static/             CSS + JS
├── tests/              MQTT simulators for testing without real Picos
├── scripts/            setup_hotspot.sh, backup_db.sh
└── systemd/            football.service unit + ops notes
```

## Hardware list

- Raspberry Pi 4 (any RAM tier) with Raspberry Pi OS **Bookworm**
- microSD card (16 GB+)
- 5 V / 3 A USB-C power supply
- Two Raspberry Pi Pico 2 W per table (firmware lives in `../src`)

## Setup (one-shot, ~10 min)

```bash
# 1. Flash Raspberry Pi OS Bookworm Lite to the SD card, boot, ssh in.

# 2. Hotspot
sudo bash scripts/setup_hotspot.sh "TableFootball" "football2026"
# (overrides: setup_hotspot.sh <ssid> <password>)

# 3. Mosquitto MQTT broker
sudo apt update
sudo apt install -y mosquitto mosquitto-clients sqlite3
sudo tee /etc/mosquitto/conf.d/football.conf >/dev/null <<'EOF'
listener 1883
allow_anonymous true
max_connections 20
EOF
# NOTE: `listener` must come FIRST. mosquitto applies any settings declared
# before a `listener` line to its implicit default listener; if `listener`
# comes last, you end up with two listeners both bound to 1883 and the
# broker fails with "Address already in use" against itself.
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto

# 4. Python env
cd /home/pi/server
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# 5. .env
cp .env.example .env
python -c "import secrets; print('SECRET_KEY=' + secrets.token_hex(32))" >> .env
nano .env             # change ADMIN_PASSWORD

# 6. systemd
sudo cp systemd/football.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable football
sudo systemctl start football

# 7. Reboot to confirm everything starts automatically.
sudo reboot
```

After reboot, connect a phone or laptop to the **TableFootball** WiFi
and open <http://192.168.4.1:5000>. The admin panel is at
<http://192.168.4.1:5000/admin>.

## Running in development (on any machine)

```bash
cd server
python3 -m venv venv
source venv/bin/activate    # on Windows: venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env        # then fill in SECRET_KEY
python app.py
```

You'll need a local mosquitto running on `localhost:1883`. On Windows
you can install one via Chocolatey (`choco install mosquitto`) or run
the simulators against a Pi over your LAN by setting `MQTT_HOST` in
`.env`.

## Try it without real Picos

In another terminal (with the server already running):

```bash
# A complete match (heartbeat → register → goals → GAME_OVER)
python tests/simulate_game.py

# Or piece-by-piece:
python tests/simulate_heartbeat.py sim-A 1 A primary
python tests/simulate_rfid.py DBEF7005 A
python tests/simulate_rfid.py 4C069804 B
```

Open the dashboard while the simulator runs to watch the table card
update live.

## MQTT topic cheat-sheet

| Direction | Topic | QoS | Retain | Notes |
|-----------|-------|-----|--------|-------|
| Pico → Pi | `tablefootball/pico/<id>/heartbeat`       | 0 | no  | every 5s |
| Pico → Pi | `tablefootball/pico/<id>/status`          | 1 | yes | LWT, `online:false` |
| Pico → Pi | `tablefootball/table/<id>/rfid`           | 1 | no  | card taps |
| Pico → Pi | `tablefootball/table/<id>/state`          | 1 | no  | score / GAME_OVER (primary only) |
| Pico → Pi | `tablefootball/table/<id>/ball`           | 0 | no  | 10 Hz ball position |
| Pi → Pico | `tablefootball/pico/<id>/player`          | 1 | no  | response after RFID |
| Pi → Pico | `tablefootball/table/<id>/sync`           | 1 | yes | canonical table state |
| Pi → Pico | `tablefootball/pico/<id>/players_list`    | 1 | yes | full {uid, name} list for offline mode |
| Pi → Pico | `tablefootball/pico/<id>/cmd`             | 1 | no  | identify / reset / message |

Manual probe (handy when wiring up Picos):

```bash
# Watch every message on the broker.
mosquitto_sub -h localhost -t 'tablefootball/#' -v

# Fake an RFID tap.
mosquitto_pub -h localhost -t tablefootball/table/1/rfid \
  -m '{"v":1,"pico_id":"sim-A","table_id":1,"side":"A","slot":1,"uid":"DBEF7005","event":"card_tapped"}'
```

## Daily operations

- **Change the admin password:** edit `.env`, then `sudo systemctl restart football`.
- **Backup the DB:** run `bash scripts/backup_db.sh` (or wait for the cron entry below).
- **Daily backups at 04:00:** `crontab -e` and add
  ```
  0 4 * * * /home/pi/server/scripts/backup_db.sh
  ```
  Backups live in `server/backups/` and are pruned after 7 days.
- **Tail logs:** `sudo journalctl -u football -f` or `tail -f logs/server.log`.

## Troubleshooting

| Symptom | First thing to check |
|---------|----------------------|
| mosquitto fails with "Address already in use" against itself | Check `/etc/mosquitto/conf.d/football.conf` — `listener 1883` must be the **first** line, before `allow_anonymous`/`max_connections`. See note in step 3 above. |
| Picos can't see the hotspot | `nmcli con show TableFootball-AP` then `nmcli con up TableFootball-AP` |
| Dashboard not updating | DevTools → Network → `/events` shows a `text/event-stream` that stays open |
| MQTT messages not arriving | `mosquitto_sub -h localhost -t 'tablefootball/#' -v` while triggering a tap |
| Server won't start | `sudo journalctl -u football -n 50` — most often a missing `SECRET_KEY` in `.env` |
| Players not appearing | confirm `pico_id` in `pico_devices` has a `table_id` assigned (or assign via /admin) |
| DB locked errors | `PRAGMA journal_mode` should be `wal`: `sqlite3 football.db "PRAGMA journal_mode"` |

## Known limitations (v1)

- Matches played while the Pico is offline are not synced back to the
  Pi when it reconnects. Offline play is supported on the Pico, but
  those matches don't reach the history.
- The server does not serve HTTPS — acceptable on the closed local
  hotspot, not for public exposure.
- Admin sessions never expire. Replace the session config (or move to
  a proper auth library) before exposing the panel outside the LAN.

## Future hooks (already wired)

- **Web NFC player lookup** — `GET /player/<uid>` and
  `GET /api/player/by-uid/<uid>` are live; a browser-side NFC scan can
  just navigate to the URL.
- **Tournaments** — `tournaments`, `tournament_entries`, and
  `tournament_matches` tables are created on first run. No UI yet, so a
  later migration is not required.
