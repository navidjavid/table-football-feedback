# Table Football Feedback System ⚽

A smart foosball table built for the UbiLab university course. The table
identifies players by RFID, tracks the ball and score in real time, shows
match status on an on-cabinet LCD per side, and streams everything live
to a web dashboard hosted on a Raspberry Pi 4 — no internet connection
required, the Pi runs its own WiFi hotspot.

This repo contains **two independently-runnable projects**:

| Part | Where | Runs on | Language |
|---|---|---|---|
| Embedded firmware | `src/`, `include/`, `lib/` | Raspberry Pi Pico 2 W ×2 | C (Pico SDK) |
| Server + dashboard | `server/` | Raspberry Pi 4 | Python (Flask) |

They talk to each other over MQTT/WiFi — but the two Picos also talk
**directly to each other** over a shared I2C bus, so registration keeps
working even with no Pi/WiFi present at all (see §3).

---

## 1. System overview

```
 ┌──────────────┐                 ┌──────────────────────┐   ┌──────────────────────┐
 │ Ball tracker │────I2C (shared)▶│  Side A PCB           │   │  Side B PCB           │
 │ (external,   │────────────────▶│  Pico 2W + RFID +     │◀─▶│  Pico 2W + RFID +     │
 │  other team) │  both boards    │  display (PRIMARY)    │I2C│  display (SECONDARY)  │
 └──────────────┘  listen         └───────────┬──────────┘   └──────────┬────────────┘
                                               │ MQTT/WiFi                │ MQTT/WiFi
                                               ▼                          ▼
                                   ┌─────────────────────────────────────────────┐
                                   │              Raspberry Pi 4                  │
                                   │  WiFi hotspot "TableFootball"                │
                                   │  Mosquitto MQTT broker                       │
                                   │  Flask web app (SSE) + SQLite (WAL)          │
                                   └───────────────────┬───────────────────────────┘
                                                       │ HTTP / SSE
                                                       ▼
                                           Any phone/laptop on the hotspot —
                                           live dashboard, player profiles,
                                           admin panel, tournament brackets
```

**Two independent PCBs, one per side** — each has its own Pico 2 W, PN532
RFID reader, and EA DOGL128 display. Both boards listen to the same
shared I2C bus from the ball-tracker hardware. Exactly one board is
configured `PICO_ROLE_PRIMARY` (owns reporting ball position/score/state
to the Pi); the other only reports its own side's RFID taps + heartbeat —
this avoids the server ever getting duplicate/racing data from both
boards for the same table.

**Data flow, end to end:**

1. A player taps an RFID card on either side's own PN532 reader.
2. That board reads the UID over SPI, registers it locally (own side's
   roster, up to 2 players for 2v2), and:
   - broadcasts it directly to the **sibling board** over the shared I2C
     bus (a board briefly becomes I2C master to send this — see §3),
     so the other side learns about it **with no Pi/WiFi involved**;
   - publishes it to the Pi over MQTT, which is authoritative for
     identity/history — it resolves the UID against its own SQLite
     database (or creates a guest), decides when the match should
     start, and echoes the resolved name back.
3. The primary board republishes ball x/y/speed/score at ~12 Hz from the
   shared I2C feed; the dashboard renders this as a moving dot on a mini
   pitch.
4. Goals are detected locally (baseline-diffed against the ball
   tracker's own score fields, not trusted directly) on **both** boards
   independently, each updating its own display; the primary board's
   published state drives the dashboard's "GOAL!" animation.
5. At game end, the Pi saves the full match (mode, score, players,
   fastest shot, winner) to SQLite — shows up in player profiles, match
   history, and tournament brackets.
6. If a table gets stuck in `GAME_PLAYING` with no update for 45s (a
   board crashed/rebooted mid-match), the Pi's own watchdog auto-abandons
   it — so a later reconnect never resumes a long-dead game.

`[PICTURE: photo of the assembled table — both enclosures, PN532s, LCDs visible]`

---

## 2. Physical components & wiring

### Hardware list (×2, one set per side)

- **Raspberry Pi Pico 2 W** — WiFi, RFID, display, local game logic
- **PN532 NFC/RFID reader module**, configured in **SPI mode** (DIP
  switches: SEL0=OFF, SEL1=ON)
- **EA DOGL128L-6** graphic LCD, 128×64, reflective (no backlight),
  driven with **bit-banged SPI** (hardware SPI caused timing issues on
  this clone)
- A custom 3D-printed enclosure (front panel + back shell,
  `hardware/enclosure/` in a sibling directory to this repo — not
  version-controlled here) with a display window and a cable
  pass-through for the shared I2C bus
- RFID cards, one per player — UID is the player's identity end to end

Plus shared infrastructure:
- An external **ball-tracker** (built by another team) that writes ball
  position packets over I2C to both boards
- **Raspberry Pi 4** (any RAM tier) + microSD (16 GB+) + 5V/3A USB-C
  power supply — runs the WiFi hotspot, MQTT broker, and Flask server

`[PICTURE: each component individually — Pico board, PN532 module with DIP switches labeled, EA DOGL128 display, an assembled enclosure, an RFID card]`

### Real-world table dimensions

The physical table is **140 cm long × 76 cm wide**. The dashboard's
field visualization is intentionally drawn at this exact 140:76 aspect
ratio (`server/static/style.css`, `.field { aspect-ratio: 140 / 76; }`),
and ball coordinates are normalized to a 0–1000 × 0–500 grid before
being sent over MQTT so the same ratio holds end to end.

`[PICTURE: top-down schematic of the table with dimensions labeled, goal positions marked]`

### Pin connections — per side board

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
| Shared ball-tracker I2C bus | SDA | GP4 | 6 |
| | SCL | GP5 | 7 |

The display additionally needs **9 capacitors** for its internal charge
pump (5×1µF on V0–V4, 3×1µF on the CAP1/2/3 chain, 4.7µF on VOUT) — see
[`docs/hardware_connections.md`](docs/hardware_connections.md) for the
full, datasheet-verified capacitor wiring table; it will not power on
without them, and the topology is easy to get wrong (this project did,
once, before it was corrected).

The shared I2C bus needs **4.7kΩ pull-ups** on both SDA and SCL, and all
boards (both side PCBs + the ball tracker) must share a common GND.

`[PICTURE: wiring diagram / breadboard schematic showing all of the above]`

### Software-side bus configuration

| Parameter | Value |
|---|---|
| SPI1 (PN532) speed | 1 MHz |
| I2C0 speed | 9600 baud |
| I2C slave address | `0x42` (shared by both side boards — see §3) |
| Display contrast | `0x13` |

Full pinout reference: [`docs/hardware_connections.md`](docs/hardware_connections.md).

---

## 3. Embedded firmware (Raspberry Pi Pico)

Built with the **Pico SDK 2.2.0**, targeting `pico2_w`, using **lwIP in
polling mode** (`NO_SYS=1` — no RTOS, everything runs from one
`while(true)` loop with `cyw43_arch_poll()` keeping WiFi/MQTT alive).

The **same firmware image** runs on both side boards — which side/role a
board plays is a compile-time config block at the top of `src/main.c`:

```c
#define MY_SIDE            'A'   // 'A' on this board, 'B' on the other
#define PICO_ROLE_PRIMARY  1     // 1 on exactly ONE board
#define PICO_ID    "pico-side-a" // must be unique per board
```

Flip these three, rebuild, and flash the other board.

### Source layout

| File | Responsibility |
|---|---|
| `src/main.c` | WiFi connect (with on-screen boot status), MQTT init, main loop |
| `src/rfid_handler.c` + `lib/pn532/pn532.c` | PN532 SPI driver + tap-debounce state machine |
| `src/i2c_comms.c` | Ball-position ring buffer **and** the offline peer-tap link between the two boards (see below) |
| `src/game_logic.c` | Local game state machine (`GAME_WAITING` → `GAME_PLAYING` → `GAME_OVER`), up to 2 players/side, goal detection, fastest-shot tracking |
| `src/display_manager.c` + `lib/ea_dogl128/` | Renders game state to the on-cabinet LCD |
| `src/pico_mqtt.c` + `include/pico_mqtt.h` | lwIP raw MQTT client wrapper — connects (with a Last-Will-Testament), publishes heartbeat/RFID/state/ball, subscribes to player/sync/rfid/cmd |
| `lwipopts.h` | lwIP buffer/pool sizing — tuned specifically to survive MQTT's keep-alive timers (see bug notes below) |

### Offline board-to-board link (no Pi/WiFi required)

Both boards need to know about **both** sides' registered players, but
cross-board MQTT sync only works when the Pi is reachable — and the PCBs
are already assembled with no spare wiring for a dedicated link. Instead,
the two boards share the same physical I2C bus the ball tracker uses:

- The ball tracker is the bus's only permanent master, always **writing**
  to address `0x42`; both boards listen there as slaves (a
  write-only-broadcast trick — see `include/i2c_comms.h`).
- When a card taps on one side, that board briefly becomes the I2C
  master itself (`i2c_slave_deinit()` → write → `i2c_slave_init()`,
  the Pico SDK's documented idiom for this) and writes a short 9-byte
  message to that same shared address. The sibling board, still
  listening normally, receives it — distinguished from ball-data packets
  purely by a different sync header.
- Standard I2C multi-master arbitration handles the rare case of
  colliding with the ball tracker's own write; a lost attempt is simply
  dropped (a tap is a one-off, low-frequency event, so this is an
  accepted low-stakes failure mode).

MQTT's `/rfid` topic (subscribed by both boards) is a second, best-effort
channel carrying the same information when the Pi is present — the I2C
link is what makes registration work **at all** with no Pi/WiFi.

### RFID → MQTT + I2C flow

1. `rfid_handler_scan()` polls the PN532 over SPI; a debounce window
   avoids re-firing on the same card sitting on the reader.
2. On a fresh tap, `game_lookup_player()` checks a small hardcoded
   table of known UIDs → names (demo roster: Alice, Bob, Carol, Dave)
   purely so the **local LCD** can show a name immediately.
3. The tap is registered locally (own side, next open slot), broadcast
   to the sibling board over I2C, and published to
   `tablefootball/table/<id>/rfid`. The Pi is the identity authority —
   it resolves the UID against its own SQLite `players` table and
   echoes the resolved name + slot back over
   `tablefootball/pico/<id>/player`.

### Ball tracking → MQTT flow

The ball tracker writes a 20-byte binary packet over I2C
(`SYNC_A SYNC_B x y prev_x prev_y field_w field_h speed possession
score_a score_b`, see `include/i2c_comms.h`). `i2c_comms_poll()` drains
this from a ring buffer filled by an I2C-slave interrupt handler — the
same ring buffer also carries the peer-tap messages above, distinguished
by sync header. `game_update()` diffs the embedded score against its
last-seen baseline to detect goals (rather than trusting the raw score
directly), and the ball position is rescaled to a 0–1000×0–500 grid
before the **primary** board publishes it on
`tablefootball/table/<id>/ball` at roughly 12 Hz.

`[PICTURE: serial console screenshot showing RFID tap + MQTT publish logs]`

### MQTT topics

**Published by each Pico:**

| Topic | Rate | Payload |
|---|---|---|
| `tablefootball/pico/<id>/heartbeat` | every 5s | online status, IP, firmware version, uptime |
| `tablefootball/table/<id>/rfid` | on tap | UID, side, slot |
| `tablefootball/table/<id>/state` | ~1/s, primary only | score, mode, fastest shot, winner (on GAME_OVER) |
| `tablefootball/table/<id>/ball` | ~12 Hz, primary only | x, y, speed |
| `tablefootball/pico/<id>/status` | LWT | `online:false`, set on disconnect |

**Subscribed by each Pico** (the Pi is source of truth for identity/state,
but the board must never depend on it being reachable — see §"Offline
board-to-board link" above):

| Topic | Purpose |
|---|---|
| `tablefootball/pico/<id>/player` | Authoritative name for a resolved tap |
| `tablefootball/table/<id>/sync` | Full roster + score/state snapshot (retained) — reconciles admin-driven registration changes, which produce no RFID tap at all |
| `tablefootball/table/<id>/rfid` | The sibling board's own taps (same topic it publishes to) |
| `tablefootball/pico/<id>/cmd` | Admin-panel commands: identify / reset_match / clear_players / show_message |

### Bugs worth knowing about (and their fixes)

These came up while building this firmware and are good examples of how
easy it is to silently break shared resources or drop a field across a
protocol boundary:

1. **I2C bytes dropped during every RFID scan.** `rfid_handler.c`
   disables `I2C0_IRQ` for the duration of `pn532_read_card()` to keep
   the I2C slave handler from interfering with SPI timing. But PN532's
   internal "wait for ready" polling loop can run for up to ~250ms per
   scan, and the IRQ stayed off that whole time — so every ball-position
   byte arriving during a card scan was lost. **Fix:** the wait loop now
   masks `I2C0_IRQ` only for the few microseconds of the actual SPI
   status read, unmasked during each 1ms sleep in between.
2. **`*** PANIC *** sys_timeout` crash.** lwIP's default
   `MEMP_NUM_SYS_TIMEOUT` pool wasn't enough once MQTT's keep-alive timer
   joined it. **Fix:** `lwipopts.h` explicitly sets it to 16.
3. **Admin dashboard commands did nothing on real hardware.** The server
   published to `tablefootball/pico/<id>/cmd` from the start, but the
   firmware never subscribed to that topic at all — every "Identify" /
   "Reset Match" / "Message" button silently no-op'd. Fixed by actually
   subscribing and dispatching (see `on_pi_cmd()` in `main.c`).
4. **A 2v2 name-correction bug.** The Pi's per-tap name resolution always
   corrected roster slot 1, no matter which slot the response was
   actually about — invisible in 1v1 (only one slot exists), but
   resolving a *second* player's name would silently overwrite the
   first player's name too. Fixed by having the server include (and the
   firmware use) the actual slot number in that message.
5. **A NULL-pointer crash with no Pi present.** If the Pi's hotspot
   simply doesn't exist at boot, `mqtt_app_init()` is never called, so
   the internal MQTT client stays `NULL` — but every heartbeat/RFID
   publish still tried to reconnect through it, crashing on first use.
   Fixed with a guard so every publish path is a safe no-op with no
   client. This is exactly the "must work fully offline" requirement,
   caught by testing the actual no-Pi scenario rather than just a
   dropped-connection one.

### Building & flashing

```bash
# Requires the Pico SDK (2.2.0) + arm toolchain + CMake + Ninja.
mkdir build && cd build
cmake -G Ninja ..
ninja table-football
```

Flip `MY_SIDE`/`PICO_ROLE_PRIMARY`/`PICO_ID` at the top of `src/main.c`
(see above), rebuild, and repeat for the other side's board.

Hold **BOOTSEL**, plug the Pico in (it mounts as a USB drive), then drag
`build/table-football.uf2` onto it.

There are also standalone test targets for bringing up hardware in
isolation without the full stack — useful when debugging a single
peripheral (see `tests/`):

| Target | Tests |
|---|---|
| `test_i2c_simulator` | Fakes ball movement over I2C — a second Pico can stand in for the real ball tracker |
| `test_pn532` | Raw PN532 SPI bring-up (firmware version + card polling), independent of the rest of the stack |
| `test_pn532_lib` | Same, using the actual `lib/pn532/pn532.c` driver the main firmware uses |
| `test_i2c_scan` | Protocol-independent I2C bus scanner (bypasses custom SPI/I2C code entirely — useful for "is this chip even alive" questions) |

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
  `matches`, `match_players`, `pico_devices`, `tournaments` /
  `tournament_entries` / `tournament_matches`.
- **A watchdog thread** — marks a Pico offline after 30s of silent
  heartbeat, and (separately) auto-abandons a table stuck in
  `GAME_PLAYING` with no `/state` update for 45s, so a board that
  crashed mid-match doesn't leave a permanently "live" game behind.

### Dashboard (`server/static/script.js`, `templates/index.html`)

- One page, no install — any device on the hotspot opens
  `http://tb.local`.
- Real-time via SSE, with an automatic 5-second polling fallback if the
  stream drops.
- Each table card shows: player name(s) per side (colored pill, with a
  Guest/Registered badge, up to 2 names per side for 2v2), live score in
  large digits, a mini pitch drawn at the table's real 140:76 ratio with
  goal markers at each end and a live ball dot, best-shot speed, and a
  winner/abandoned banner.
- **Goal celebration:** when a side's score increases, a 2-second
  animated "GOAL!" banner pops over the pitch, colored and named for
  the scoring side.
- Clicking a row in the players table opens that player's full profile
  page (`/player/<uid>`) — match history and a fastest-shot-over-time
  chart.

`[PICTURE: screenshot of the dashboard mid-match, ideally caught mid-goal-animation]`

### Player profiles, admin & tournaments

- `/player/<uid>` — games/wins/losses, best shot, full match history,
  and (browser permitting) a fastest-shot-over-time chart.
- `/admin` — rename guest players into registered ones, watch Pico
  online/offline status, per-Pico commands (identify/reset/clear/message —
  see §3), and a tournament manager: create a bracket, add entries,
  auto-generate a single-elimination tree (byes go to the top seeds,
  never bye-vs-bye), assign a pending match to a live table, and watch
  it auto-advance as matches finish. Basic auth via `.env`'s
  `ADMIN_PASSWORD`.

`[PICTURE: screenshot of a player profile page, the admin panel, and a tournament bracket]`

### Try it without real Picos

```bash
cd server
python tests/simulate_game.py          # full match: heartbeat → register → goals → GAME_OVER
python tests/simulate_rfid.py DBEF7005 A
```

---

## 5. Known limitations / next steps

- **Offline match history sync** — a match played entirely while a Pico
  is disconnected from the Pi never reaches match history/stats once it
  reconnects (registration itself works fully offline via the I2C peer
  link in §3; only the *server-side record* of that match is missing).
- **Local "tap out" while offline** — tapping an already-seated card is
  meant to deregister that player (the server already treats a repeat
  tap this way); the local firmware currently just ignores a repeat tap
  as a duplicate, so leaving mid-game only works once the Pi is
  reachable to correct it via `sync`.
- **Self-service player registration** — a truly unknown card currently
  becomes a "Guest" automatically; naming a guest still requires the
  admin panel rather than a prompt on the dashboard itself.
- **Real ball tracking** — the ball-tracker hardware is owned by another
  team; this repo only defines the I2C packet format it must produce.
- **HTTPS / session expiry** — fine on a closed local hotspot, but would
  need hardening before exposing the admin panel beyond the LAN.

`[PICTURE: optional roadmap graphic]`

---

## Repository layout

```
table-football/
├── src/                  Pico firmware: main.c, rfid_handler.c, i2c_comms.c,
│                         game_logic.c, display_manager.c, pico_mqtt.c
├── include/              Public headers for the above
├── lib/
│   ├── pn532/            PN532 SPI driver
│   └── ea_dogl128/       EA DOGL128 display driver
├── tests/                Standalone Pico test harnesses (see §3 table)
├── docs/
│   ├── hardware_connections.md   Full pinout + capacitor wiring reference
│   └── hardware_setup.md
├── lwipopts.h            lwIP buffer/pool sizing (tuned for MQTT keep-alive)
├── CMakeLists.txt        Builds table-football + all standalone test targets
└── server/               Flask + MQTT + SQLite server and dashboard
    ├── app.py, mqtt_client.py, database.py, state.py, config.py
    ├── templates/, static/        Dashboard, player profile, admin UI
    ├── tests/                     MQTT simulators (run the server without real Picos)
    ├── scripts/                   setup_hotspot.sh, backup_db.sh
    └── systemd/                   football.service for boot-time autostart
```

The 3D-printable enclosure design (`enclosure.scad` + exported STLs)
lives in a sibling `hardware/enclosure/` directory outside this repo,
not version-controlled here.

## Prerequisites

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0 + arm-none-eabi
  toolchain + CMake + Ninja, for the firmware.
- Python 3.11+ and Mosquitto, for the server (see
  [`server/README.md`](server/README.md) for the full Raspberry Pi setup).
