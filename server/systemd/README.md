# systemd unit

`football.service` runs the Flask server under the `pi` user. It depends
on `mosquitto.service` so the MQTT broker is up before the app starts.

## Install

```bash
sudo cp systemd/football.service /etc/systemd/system/football.service
sudo systemctl daemon-reload
sudo systemctl enable football
sudo systemctl start football
```

## Operate

```bash
sudo systemctl status football          # is it running?
sudo systemctl restart football         # after pulling new code / editing .env
sudo journalctl -u football -f          # tail the live log
sudo journalctl -u football --since "1 hour ago"
```

The app also writes `server/logs/server.log` (rotating, 10 MB × 5
backups). journalctl is easiest for spotting startup failures; the file
log is where you'll find per-message detail at `LOG_LEVEL=DEBUG`.

## Adjusting paths

The unit assumes the server folder lives at `/home/pi/Desktop/server` and
the virtualenv at `/home/pi/Desktop/server/venv`. If you install elsewhere, edit
`WorkingDirectory`, `EnvironmentFile`, and `ExecStart` accordingly and
re-run `daemon-reload`.

## Required environment

`EnvironmentFile=/home/pi/server/.env` — at minimum it must set
`ADMIN_PASSWORD` and `SECRET_KEY`. The service refuses to start
correctly without `SECRET_KEY` (sessions would reset on every restart).

## Binding port 80

`SERVER_PORT=80` (the default, see `.env.example`) lets the dashboard be
reached without a port in the URL, e.g. `http://tb.local`. Only root can
normally bind ports below 1024, and the service runs as `pi` — the unit
grants just that one capability via `AmbientCapabilities=CAP_NET_BIND_
SERVICE` instead of running the whole app as root. Nothing extra to set
up: it's already in `football.service`, works with `NoNewPrivileges=true`
right above it, and survives recreating the venv (unlike `setcap` on the
python binary, which would need re-applying every time and — since the
venv's `python3` is a symlink to the system-wide `/usr/bin/python3` —
would grant the capability to every Python process on the Pi, not just
this service).

Set `SERVER_PORT=5000` in `.env` instead if you'd rather skip binding a
privileged port and use `:5000` in the URL.

## mosquitto

The broker config lives at `/etc/mosquitto/conf.d/football.conf`:

```
allow_anonymous true
max_connections 20
listener 1883 0.0.0.0
```

Then:

```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
```
