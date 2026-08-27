#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "i2c_comms.h"

#define MAX_SCORE            5
#define NAME_LEN             16
#define MAX_PLAYERS_PER_SIDE 2

typedef enum {
    GAME_WAITING,   // fewer than 1 registered player on at least one side
    GAME_PLAYING,
    GAME_OVER,
} GameState;

typedef struct {
    bool    filled;
    char    name[NAME_LEN];
    uint8_t uid[4];
} PlayerSlot;

typedef struct {
    GameState state;

    PlayerSlot side_a[MAX_PLAYERS_PER_SIDE];
    PlayerSlot side_b[MAX_PLAYERS_PER_SIDE];

    // Game's own score counters — incremented only on goal events
    uint8_t score_a;
    uint8_t score_b;

    // Baseline from first packet (to detect goals, not use raw sim score)
    uint8_t  _last_ball_score_a;
    uint8_t  _last_ball_score_b;
    bool     _score_initialized;

    // Goal animation — time-based
    uint32_t goal_anim_end_ms;  // epoch ms when animation ends (0=none)
    int      goal_scorer;       // 1=A, 2=B

    // Live data
    float    fastest_kmh;
    float    current_kmh;
    uint8_t  possession;
    uint16_t ball_x, ball_y;
    uint16_t field_w, field_h;

    // Timing
    uint32_t game_start_ms;
    uint32_t game_end_ms;
    int      winner;   // 0=none 1=side A 2=side B
} GameData;

const char* game_lookup_player(const uint8_t uid[4]);
void        game_init(GameData *g);

// side must be 'A' or 'B'. Fills the next open slot for that side (up to
// MAX_PLAYERS_PER_SIDE). A repeat tap of a UID already seated on either
// side is ignored. The game moves WAITING -> PLAYING the moment BOTH
// sides have at least one filled slot — it never waits for a second
// player on either side. A side may still pick up its second player at
// any time afterwards, including mid-game (2v1/1v2 are valid throughout).
void game_register_player(GameData *g, char side, const uint8_t uid[4],
                           const char *name);
void game_update(GameData *g, const BallData *ball);
bool game_goal_animating(const GameData *g);
const char* game_state_label(GameState s);
uint32_t    game_elapsed_seconds(const GameData *g);

// Directly sets one roster slot's contents to match the Pi's authoritative
// view — used to reconcile admin-driven registration changes (add/remove
// player from the dashboard, or a forced admin "start"), which produce no
// RFID tap and so would otherwise never reach this board's local state at
// all. Unlike game_register_player() this bypasses de-dup/slot-picking:
// it sets exactly the slot given. uid may be NULL when filled is false.
void game_sync_roster_slot(GameData *g, char side, int slot, bool filled,
                           const char *name, const uint8_t uid[4]);

// Call after one or more game_sync_roster_slot() calls: transitions
// WAITING -> PLAYING if both sides now have at least a slot 1, exactly
// like a normal tap-driven start would (resets scores/timers). No-op if
// already playing/over or still short a side.
void game_recheck_start(GameData *g);

// Builds a display label for one side's roster into `out` (>= 34 bytes):
// "---" if empty, "Alice" for one player, "Alice+Bob" for two.
void game_side_label(const GameData *g, char side, char *out, size_t out_sz);
