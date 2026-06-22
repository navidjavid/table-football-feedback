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

The unit assumes the server folder lives at `/home/pi/server` and the
virtualenv at `/home/pi/server/venv`. If you install elsewhere, edit
`WorkingDirectory`, `EnvironmentFile`, and `ExecStart` accordingly and
re-run `daemon-reload`.

## Required environment

`EnvironmentFile=/home/pi/server/.env` — at minimum it must set
`ADMIN_PASSWORD` and `SECRET_KEY`. The service refuses to start
correctly without `SECRET_KEY` (sessions would reset on every restart).

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
