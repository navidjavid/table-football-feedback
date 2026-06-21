# Server Dashboard Plan

This folder is reserved for the Raspberry Pi server implementation.

The current project already has Pico firmware that sends live game data to a server endpoint using HTTP POST. The next step is to implement the Raspberry Pi side here.

## Final Dashboard Scope

The dashboard should stay simple and use only one page.

It should include:

- Live Match section
- All Players list
- Player Profile dialog with match history
- Register Guest dialog

It should not include:

- Leaderboard
- Recent matches section on the main dashboard
- Total shots
- Last goal
- Possession
- Ball position

## Desired Server Folder Structure

```text
server/
├── app.py
├── football.db              # generated automatically
├── templates/
│   └── index.html
├── static/
│   ├── style.css
│   └── script.js
└── README.md
```

## System Idea

The Pico keeps the embedded game logic:

- RFID scan
- I2C game/ball data reception
- score calculation
- fastest shot calculation
- winner detection
- display handling
- sending live game state to the Raspberry Pi server

The Raspberry Pi server handles:

- SQLite database
- web dashboard
- guest player creation
- player registration
- player profile/history
- saving finished matches

## Player Flow

### 1. New RFID tag is scanned

The Pico sends the RFID UID to the server as part of the `/update` JSON.

Example:

```json
{
  "state": "GAME_PLAYING",
  "p1": "Guest",
  "p1_uid": "DBEF7005",
  "p2": "Guest",
  "p2_uid": "4C069804",
  "score_a": 3,
  "score_b": 2,
  "fastest": 12.4,
  "winner": "",
  "winner_uid": "",
  "time": 80
}
```

### 2. Server checks the database

For each RFID UID:

- If the UID already exists, use the existing player.
- If the UID does not exist, create a guest player automatically.

Example:

```text
UID DBEF7005 does not exist
→ create Guest1
```

### 3. Guest can play immediately

The dashboard shows the player as `Guest1`.

The guest can play matches and all statistics are saved.

### 4. Guest registers later

The user clicks the guest row in the All Players list.

The Player Profile dialog opens.

If the player is a guest, the dialog shows:

```text
Register This Player
```

When clicked, the Register dialog opens:

```text
RFID UID: DBEF7005
Current Name: Guest1
New Name: Sara
```

After saving:

```text
Guest1 → Sara
is_guest = false
```

Important: do not create a new player. Update the same database row so all old history remains connected.

## Database Design

Use SQLite.

### players

```sql
CREATE TABLE IF NOT EXISTS players (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    rfid_uid TEXT UNIQUE NOT NULL,
    name TEXT NOT NULL,
    is_guest INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL
);
```

### matches

```sql
CREATE TABLE IF NOT EXISTS matches (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    player1_id INTEGER NOT NULL,
    player2_id INTEGER NOT NULL,
    score1 INTEGER NOT NULL,
    score2 INTEGER NOT NULL,
    winner_id INTEGER,
    fastest_shot REAL,
    started_at TEXT,
    ended_at TEXT
);
```

### live_state

```sql
CREATE TABLE IF NOT EXISTS live_state (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    state TEXT,
    player1_id INTEGER,
    player2_id INTEGER,
    score1 INTEGER,
    score2 INTEGER,
    fastest_shot REAL,
    winner_id INTEGER,
    updated_at TEXT
);
```

## Required API Endpoints

### `POST /update`

Receives data from the Pico.

Behavior:

- Read `p1_uid` and `p2_uid`.
- Find or create players.
- Update `live_state`.
- If `state == GAME_OVER`, save a match in `matches`.
- Avoid saving the same finished match repeatedly, because the Pico may send `GAME_OVER` every second.

### `GET /api/live`

Returns current live match information.

Example:

```json
{
  "state": "GAME_PLAYING",
  "player1": {
    "id": 1,
    "name": "Sara",
    "rfid_uid": "DBEF7005",
    "is_guest": false
  },
  "player2": {
    "id": 2,
    "name": "Ali",
    "rfid_uid": "4C069804",
    "is_guest": false
  },
  "score1": 3,
  "score2": 2,
  "fastest_shot": 12.4,
  "winner": null,
  "updated_at": "2026-06-21 12:45:30"
}
```

### `GET /api/players`

Returns all players with calculated statistics.

Each player should include:

```json
{
  "id": 1,
  "name": "Sara",
  "rfid_uid": "DBEF7005",
  "is_guest": false,
  "games": 5,
  "wins": 3,
  "losses": 2,
  "best_shot": 12.4,
  "status": "Active"
}
```

Statistics:

- `games`: number of matches where player is player1 or player2
- `wins`: number of matches where winner_id is this player
- `losses`: games - wins
- `best_shot`: maximum fastest_shot from matches where this player participated
- `status`: Active if currently player1 or player2 in live_state and game is running, otherwise Offline

### `GET /api/player/<player_id>`

Returns one player profile and previous match history.

Example:

```json
{
  "id": 1,
  "name": "Sara",
  "rfid_uid": "DBEF7005",
  "is_guest": false,
  "games": 5,
  "wins": 3,
  "losses": 2,
  "best_shot": 12.4,
  "matches": [
    {
      "date": "21 Jun 2026 14:30",
      "opponent": "Ali",
      "score": "5 - 3",
      "result": "Win",
      "fastest_shot": 12.4
    }
  ]
}
```

The matches array should only include matches where this player participated. Sort newest first.

### `POST /api/register-player`

Registers a guest player.

Input:

```json
{
  "player_id": 3,
  "name": "Sara"
}
```

Behavior:

- Find the player by ID.
- Update the same row.
- Set `name` to the entered name.
- Set `is_guest = 0`.
- Do not create a new player.
- Keep all previous matches and statistics.

Validation:

- Name must not be empty.
- Trim spaces.
- If another registered player already uses the same name, return an error.

## Frontend Requirements

Use plain HTML, CSS, and JavaScript.

No React, no Vue, no complex frontend framework.

### Main Page

The dashboard should have only one page.

Sections:

1. Header
2. Live Match card
3. All Players table/list

### Live Match Card

Show:

- Player 1 name
- Player 2 name
- Score
- Status
- Fastest Shot
- Winner

Do not show:

- possession
- total shots
- last goal
- ball position

### All Players List

Columns:

- Player
- RFID UID
- Games
- Wins
- Best Shot
- Status

Each player row should be clickable.

Clicking a row opens the Player Profile dialog.

Guest players should have a yellow `Guest` badge.

Registered players should have a green `Registered` badge.

### Player Profile Dialog

Show:

- Name
- Registered/Guest badge
- RFID UID
- Games
- Wins
- Losses
- Best Shot
- Match History table

If the selected player is a guest, show:

```text
Register This Player
```

Clicking that opens the Register dialog.

### Register Dialog

Fields:

- RFID UID, read-only
- Current name, read-only
- New player name, input
- Cancel button
- Save button

On Save:

- Call `POST /api/register-player`
- Close register dialog
- Refresh players list
- Refresh live match
- Refresh profile dialog if it is still open

## JavaScript Requirements

In `static/script.js`, implement:

- `loadLive()`
- `loadPlayers()`
- `openPlayerProfile(playerId)`
- `openRegisterDialog(player)`
- `submitRegistration()`
- `closeModal()`

Polling:

- `/api/live` every 1 second
- `/api/players` every 3 seconds

Use `fetch()`.

## UI Style

Use a modern dark dashboard style:

- dark background
- rounded cards
- green accent for active status
- purple accent for fastest shot
- blue/red player colors
- yellow guest badge
- green registered badge
- hover effect on player rows
- centered or side-panel modal dialog

## Running the Server

```bash
cd server
python3 -m venv venv
source venv/bin/activate
pip install flask
python app.py
```

Open in browser:

```text
http://<raspberry-pi-ip>:5000
```

The Pico should send to:

```text
http://<raspberry-pi-ip>:5000/update
```
