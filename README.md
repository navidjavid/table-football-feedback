# Table Football Feedback System ⚽

A smart foosball table built for the UbiLab university course. The table
identifies players by RFID, tracks the ball and score in real time, shows
match status on an on-cabinet LCD, and streams everything live to a
web dashboard hosted on a Raspberry Pi 4 — no internet connection
required, the Pi runs its own WiFi hotspot.

This repo contains **two independently-runnable projects**:

| Part | Where | Runs on | Language |
|---|---|---|---|
| Embedded firmware | `src/`, `include/`, `lib/` | Raspberry Pi Pico 2 W | C (Pico SDK) |
| Server + dashboard | `server/` | Raspberry Pi 4 | Python (Flask) |

The two talk to each other over MQTT, over WiFi, with no wires between
the Pico and the Pi.

---

## 1. System overview

```
 ┌────────────────────┐        ┌─────────────────────┐        MQTT/WiFi       ┌──────────────────────────────┐
 │  Simulator Pico     │  I2C   │   Main Pico 2 W      │ ───────────────────▶  │        Raspberry Pi 4         │
 │  (ball tracking)    │ ─────▶ │  RFID + game logic   │                       │  WiFi hotspot "TableFootball" │
 │                      │ 9600  │  + LCD display       │ ◀───────────────────  │  Mosquitto MQTT broker        │
 └────────────────────┘ baud   └─────────────────────┘                       │  Flask web app (SSE)          │
                                                                              │  SQLite (WAL) database        │
                                                                              └───────────────┬───────────────┘
                                                                                              │ HTTP / SSE
                                                                                              ▼
                                                                                  Any phone/laptop on the
                                                                                  hotspot — live dashboard,
                                                                                  player profiles, admin panel
```

**Data flow, end to end:**

1. A player taps an RFID card on the PN532 reader wired to the main Pico.
2. The main Pico reads the card's UID over SPI, looks it up locally (for
   the on-cabinet LCD), and publishes the tap over MQTT to the Pi.
3. The Pi (`mqtt_client.py`) is the **source of truth** for game state —
   it registers the player against the SQLite database, decides when
   both sides are full and the match auto-starts, and broadcasts the
   updated table state to every connected browser via Server-Sent
   Events (SSE).
4. A second Pico continuously reports ball x/y/speed/score over I2C to
   the main Pico, which republishes it over MQTT roughly 12 times a
   second. The dashboard renders this as a moving dot on a mini pitch.
5. When the score changes, the main Pico detects the goal locally,
   updates its LCD, and the next state publish carries the new score to
   the Pi — which fires a "GOAL!" animation on every connected
   dashboard.
6. At game end, the Pi saves the full match (mode, score, players,
   fastest shot, winner) to SQLite, so it shows up forever after in
   player profiles and match history.

This demo runs with **one Pico for one table** (not the full two-Pico
per-table design the protocol supports) — the same Pico's PN532 reader
takes the first tap as side A and the second as side B.

`[PICTURE: photo of the assembled table — cabinet open, both Picos, PN532, LCD visible]`

`[PICTURE: architecture diagram redrawn as a clean image for slides — see ASCII version above]`

---

## 2. Physical components & wiring

### Hardware list

- **Raspberry Pi Pico 2 W** ×2 — one is the *main* controller (WiFi, RFID,
  display, game logic), the other is the *simulator/tracker* that feeds
  ball position over I2C
- **PN532 NFC/RFID reader module**, configured in **SPI mode** (DIP
  switches: SEL0=OFF, SEL1=ON)
- **EA DOGL128L-6** graphic LCD, 128×64, reflective (no backlight),
  driven with **bit-banged SPI** (hardware SPI caused timing issues on
  this clone)
- RFID cards, one per player — UID is the player's identity end to end
- **Raspberry Pi 4** (any RAM tier) + microSD (16 GB+) + 5V/3A USB-C
  power supply — runs the WiFi hotspot, MQTT broker, and Flask server

`[PICTURE: each component individually — Pico board, PN532 module with DIP switches labeled, EA DOGL128 display, an RFID card]`

### Real-world table dimensions

The physical table is **140 cm long × 76 cm wide**. The dashboard's
field visualization is intentionally drawn at this exact 140:76 aspect
ratio (`server/static/style.css`, `.field { aspect-ratio: 140 / 76; }`),
and ball coordinates are normalized to a 0–1000 × 0–500 grid before
being sent over MQTT so the same ratio holds end to end.

`[PICTURE: top-down schematic of the table with dimensions labeled, goal positions marked]`

### Pin connections — main Pico

| Component | Signal | Pico GPIO | Physical pin |
|---|---|---|---|
| PN532 (SPI1) | SCK | GP10 | 14 |
| | MOSI | GP11 | 15 |
| | MISO | GP12 | 16 |
| | NSS/CS | GP13 | 17 |
| | RSTO | GP15 | 20 |
| | VCC / GND | 3.3V / GND | 36 / 38 |
| EA DOGL128 (bit-bang SPI) | SI (MOSI) | GP19 | 25 |
| | SCL (SCK) | GP18 | 24 |
| | A0 (data/cmd) | GP20 | 26 |
| | RST | GP21 | 27 |
| | CS1B | GP17 | 22 |
| I2C bus to simulator Pico | SDA | GP4 | 6 |
| | SCL | GP5 | 7 |

The display additionally needs **9 capacitors** for its internal charge
pump (1µF ceramic ×8 + 4.7µF electrolytic on VOUT) — see
[`docs/hardware_connections.md`](docs/hardware_connections.md) for the
full capacitor wiring table, it will not power on without them.

The I2C bus needs **4.7kΩ pull-ups** on both SDA and SCL (place on
either board), and both Picos must share a common GND. The simulator
Pico can be powered straight from the main Pico's VBUS → VSYS.

`[PICTURE: wiring diagram / breadboard schematic showing all of the above]`

### Software-side bus configuration

| Parameter | Value |
|---|---|
| SPI1 (PN532) speed | 1 MHz |
| I2C0 speed | 9600 baud |
| I2C slave address | `0x42` |
| Display contrast | `0x13` |

Full pinout reference: [`docs/hardware_connections.md`](docs/hardware_connections.md).

> Note: `lib/mfrc522/` and the MFRC522-based pins mentioned in older
> docs are **legacy** — the project switched to the PN532 reader. The
> MFRC522 driver is kept in-tree for reference only and is not built by
> `CMakeLists.txt`.

---

## 3. Embedded firmware (Raspberry Pi Pico)

Built with the **Pico SDK 2.2.0**, targeting `pico2_w`, using **lwIP in
polling mode** (`NO_SYS=1` — no RTOS, everything runs from one
`while(true)` loop with `cyw43_arch_poll()` keeping WiFi/MQTT alive).

### Source layout

| File | Responsibility |
|---|---|
| `src/main.c` | WiFi connect, MQTT init, main loop: poll I2C → scan RFID → render display → publish heartbeat/state/ball at their own rates |
| `src/rfid_handler.c` + `lib/pn532/pn532.c` | PN532 SPI driver + tap-debounce state machine |
| `src/i2c_comms.c` | I2C slave — receives ball-position packets into a ring buffer from the simulator Pico |
| `src/game_logic.c` | Local game state machine (`GAME_REGISTER_P1` → `GAME_REGISTER_P2` → `GAME_PLAYING` → `GAME_OVER`), goal detection, fastest-shot tracking, local known-player lookup table |
| `src/display_manager.c` + `lib/ea_dogl128/` | Renders game state to the on-cabinet LCD |
| `src/pico_mqtt.c` + `include/pico_mqtt.h` | Thin wrapper around lwIP's raw MQTT client — connection-state tracking with backoff, publish helpers for heartbeat/RFID/state/ball |
| `lwipopts.h` | lwIP buffer/pool sizing — tuned specifically to survive MQTT's keep-alive timers (see bug notes below) |

### Demo scope (one Pico)

This build runs **one Pico for one table**, not the full two-Pico
architecture the MQTT protocol is designed for. `main.c` hardcodes
`PICO_ID = "pico-demo"`, `TABLE_ID = 1`. The same PN532 reader is used
for both sides: the **first** tap after registration/game-over starts a
new side-A registration, the **second** tap fills side B and
auto-starts the match.

### RFID → MQTT flow

1. `rfid_handler_scan()` polls the PN532 over SPI; a debounce window
   (`NO_CARD_THRESHOLD`) avoids re-firing on the same card sitting on
   the reader.
2. On a fresh tap, `game_lookup_player()` checks a small hardcoded
   table of known UIDs → names (the demo roster: Alice, Bob, Carol,
   Dave) purely so the **local LCD** can show a name immediately.
3. The UID is published to `tablefootball/table/<id>/rfid`. The Pi is
   the authority — it looks the player up (or creates a guest) in its
   own SQLite `players` table independently of the Pico's local list.

### Ball tracking → MQTT flow

The simulator Pico sends a 20-byte binary packet over I2C
(`SYNC_A SYNC_B x y prev_x prev_y field_w field_h speed possession
score_a score_b`, see `include/i2c_comms.h`). `i2c_comms_poll()` drains
this from a ring buffer filled by an I2C-slave interrupt handler.
`game_update()` diffs the embedded score against its last-seen baseline
to detect goals (rather than trusting the simulator's raw score
directly), and the ball position is rescaled to a 0–1000×0–500 grid
before being published on `tablefootball/table/<id>/ball` at roughly
12 Hz.

`[PICTURE: serial console screenshot showing RFID tap + MQTT publish logs]`

### MQTT topics published by the Pico

| Topic | Rate | Payload |
|---|---|---|
| `tablefootball/pico/<id>/heartbeat` | every 5s | online status, IP, firmware version, uptime |
| `tablefootball/table/<id>/rfid` | on tap | UID, side, slot |
| `tablefootball/table/<id>/state` | ~1/s while playing | score, mode, fastest shot, winner (on GAME_OVER) |
| `tablefootball/table/<id>/ball` | ~12 Hz | x, y, speed |

### Two bugs worth knowing about (and their fixes)

These came up while building this firmware and are good examples of
how easy it is to silently break shared resources between peripherals:

1. **I2C bytes dropped during every RFID scan.** `rfid_handler.c`
   disables `I2C0_IRQ` for the duration of `pn532_read_card()` to keep
   the I2C slave handler from interfering with SPI timing. But PN532's
   internal "wait for ready" polling loop (`_wait_ready()` in
   `lib/pn532/pn532.c`) can run for up to ~250ms per scan, and the I2C
   IRQ stayed off that whole time — so every ball-position byte arriving
   during a card scan was lost. **Fix:** `_wait_ready()` now masks
   `I2C0_IRQ` only for the few microseconds of the actual SPI status
   read, and leaves it unmasked during each 1ms sleep in between.
2. **`*** PANIC *** sys_timeout` crash.** lwIP's default
   `MEMP_NUM_SYS_TIMEOUT` pool (auto-sized to ~5) wasn't enough once
   MQTT's keep-alive timer joined the pool. **Fix:** `lwipopts.h`
   explicitly sets `MEMP_NUM_SYS_TIMEOUT=16` and bumps `MEM_SIZE`/
   `MEMP_NUM_TCP_PCB` accordingly.

### Building & flashing

```bash
# Requires the Pico SDK (2.2.0) + arm toolchain + CMake + Ninja.
mkdir build && cd build
cmake -G Ninja ..
ninja table-football
```

Hold **BOOTSEL**, plug the Pico in (it mounts as a USB drive), then
drag `build/table-football.uf2` onto it.

There's also a separate `test_i2c_simulator` target (`tests/test_i2c_simulator.c`)
for flashing the second Pico that fakes ball movement over I2C —
useful for testing the main Pico/dashboard without a real
camera/sensor rig.

---

## 4. Server & dashboard (Raspberry Pi 4)

Everything in `server/` — see [`server/README.md`](server/README.md)
for the full setup walkthrough, troubleshooting table, and MQTT
cheat-sheet. Summary:

### Stack

- **NetworkManager hotspot** — the Pi broadcasts its own WiFi network
  (`TableFootball`), so the whole system works with zero internet/router
  dependency.
- **Mosquitto** — local MQTT broker on port 1883.
- **Flask** (`server/app.py`) — HTTP routes, admin auth, and an
  `/events` SSE endpoint that streams `table_state`, `ball_position`,
  `player_update`, and `pico_status` events to every open browser.
- **`mqtt_client.py`** — subscribes to `tablefootball/#`, owns the
  authoritative game state machine per table:
  `WAITING → PLAYERS_REGISTERING → GAME_PLAYING → GAME_OVER / ABANDONED`,
  and writes everything through to SQLite.
- **SQLite (WAL mode)** — `players`, `live_tables`, `live_players`,
  `matches`, `match_players`, `pico_devices`, plus unused-but-ready
  `tournaments`/`tournament_entries`/`tournament_matches` tables.

### Dashboard (`server/static/script.js`, `templates/index.html`)

- One page, no install — any device on the hotspot opens
  `http://192.168.4.1:5000`.
- Real-time via SSE, with an automatic 5-second polling fallback if the
  stream drops.
- Each table card shows: player name(s) per side (colored pill, with a
  Guest/Registered badge), live score in large digits, a mini pitch
  drawn at the table's real 140:76 ratio with goal markers at each end
  and a live ball dot, best-shot speed, and a winner/abandoned banner.
- **Goal celebration:** when a side's score increases, a 2-second
  animated "GOAL!" banner pops over the pitch, colored and named for
  the scoring side — it survives even the match-winning goal's
  transition into the game-over view.

`[PICTURE: screenshot of the dashboard mid-match, ideally caught mid-goal-animation]`

### Player profiles & admin

- Tapping a row in the players table (or visiting `/player/<uid>`)
  opens a profile: games/wins/losses, best shot, full match history,
  and (browser permitting) a fastest-shot-over-time chart.
- `/admin` — rename guest players into registered ones, watch Pico
  online/offline status, basic auth via `.env`'s `ADMIN_PASSWORD`.

`[PICTURE: screenshot of a player profile page and the admin panel]`

### A bug worth knowing about: stale rosters after GAME_OVER

`live_players` (the per-table roster) used to persist after a match
ended — kept around on purpose so the GAME_OVER banner could show who
just played. But that meant the **first** RFID tap of the *next* game
was misread: the server saw an existing entry for that UID and treated
the tap as a "deregister" instead of a fresh registration (or, if a
different player tapped, found the side "full" and silently ignored
it). **Fix:** in `mqtt_client.py`, any tap arriving while a table is in
`GAME_OVER`/`ABANDONED` now clears the roster and resets to `WAITING`
before processing the tap, so the new game always starts clean.

### Try it without real Picos

```bash
cd server
python tests/simulate_game.py          # full match: heartbeat → register → goals → GAME_OVER
python tests/simulate_rfid.py DBEF7005 A
```

---

## 5. Future improvements

- **Scale to the full two-Pico-per-table design** the MQTT protocol
  already supports (one Pico per side), instead of one Pico handling
  both sides for this demo.
- **Self-service player registration** — tap an unknown card and
  register a name from the dashboard, instead of relying on the
  hardcoded demo roster (`Alice`/`Bob`/`Carol`/`Dave`) baked into
  `game_logic.c`.
- **Offline match sync** — matches played while a Pico is disconnected
  from the Pi currently never reach match history once it reconnects.
- **2v2 polish + tournaments** — the `tournaments` tables already exist
  in the schema; no UI has been built on top of them yet.
- **Real ball tracking** — replace the I2C simulator Pico with an
  actual sensor/vision-based tracker reporting the same packet format.
- **HTTPS / session expiry** — fine on a closed local hotspot, but
  would need hardening before exposing the admin panel beyond the LAN.

`[PICTURE: optional roadmap graphic]`

---

## Repository layout

```
table-football/
├── src/                  Pico firmware: main.c, rfid_handler.c, i2c_comms.c,
│                         game_logic.c, display_manager.c, pico_mqtt.c
├── include/              Public headers for the above
├── lib/
│   ├── pn532/            PN532 SPI driver (in use)
│   ├── ea_dogl128/       EA DOGL128 display driver (in use)
│   └── mfrc522/          Legacy RFID driver — not built, kept for reference
├── tests/                Pico-side test harnesses (display, RFID, I2C simulator)
├── docs/
│   └── hardware_connections.md   Full pinout + capacitor wiring reference
├── lwipopts.h            lwIP buffer/pool sizing (tuned for MQTT keep-alive)
├── CMakeLists.txt        Builds `table-football` + `test_i2c_simulator`
└── server/               Flask + MQTT + SQLite server and dashboard
    ├── app.py, mqtt_client.py, database.py, state.py, config.py
    ├── templates/, static/        Dashboard, player profile, admin UI
    ├── tests/                     MQTT simulators (run the server without real Picos)
    ├── scripts/                   setup_hotspot.sh, backup_db.sh
    └── systemd/                   football.service for boot-time autostart
```

## Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0 + arm-none-eabi
  toolchain + CMake + Ninja, for the firmware.
- Python 3.11+ and Mosquitto, for the server (see
  [`server/README.md`](server/README.md) for the full Raspberry Pi setup).
