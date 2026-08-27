#include "game_logic.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Known players — local best-guess only. The Pi's player database is
// authoritative; main.c's on_pi_player() corrects a seated name here once
// the server resolves the tap against its real records.
// ---------------------------------------------------------------------------
typedef struct { uint8_t uid[4]; const char *name; } KnownPlayer;

static const KnownPlayer _known[] = {
    { { 0xDB, 0xEF, 0x70, 0x05 }, "Alice" },
    { { 0x4C, 0x06, 0x98, 0x04 }, "Bob"   },
    { { 0xDB, 0x4E, 0x6C, 0x05 }, "Carol" },
    { { 0xC2, 0x87, 0x87, 0x04 }, "Dave"  },
};
static const int _n = sizeof(_known) / sizeof(_known[0]);

const char* game_lookup_player(const uint8_t uid[4]) {
    for (int i = 0; i < _n; i++)
        if (memcmp(_known[i].uid, uid, 4) == 0)
            return _known[i].name;
    return NULL;
}

void game_init(GameData *g) {
    memset(g, 0, sizeof(*g));
    g->state = GAME_WAITING;
}

static PlayerSlot* _side_slots(GameData *g, char side) {
    return (side == 'A') ? g->side_a : g->side_b;
}

static bool _side_has_uid(const PlayerSlot slots[MAX_PLAYERS_PER_SIDE],
                           const uint8_t uid[4]) {
    for (int i = 0; i < MAX_PLAYERS_PER_SIDE; i++)
        if (slots[i].filled && memcmp(slots[i].uid, uid, 4) == 0)
            return true;
    return false;
}

static bool _side_ready(const PlayerSlot slots[MAX_PLAYERS_PER_SIDE]) {
    return slots[0].filled;
}

static void _start_playing(GameData *g) {
    g->state              = GAME_PLAYING;
    g->score_a            = 0;
    g->score_b            = 0;
    g->fastest_kmh        = 0;
    g->goal_anim_end_ms   = 0;
    g->goal_scorer        = 0;
    g->_score_initialized = false;
    g->game_start_ms      = to_ms_since_boot(get_absolute_time());
    g->game_end_ms        = 0;
    g->winner             = 0;
    printf("[GAME] Both sides ready -> PLAYING\n");
}

void game_register_player(GameData *g, char side, const uint8_t uid[4],
                           const char *name) {
    if (side != 'A' && side != 'B') return;

    // A tap after GAME_OVER starts a fresh match: clear everyone and
    // re-seat this tap as the first player of its side.
    if (g->state == GAME_OVER) {
        game_init(g);
    }

    PlayerSlot *mine   = _side_slots(g, side);
    PlayerSlot *theirs = _side_slots(g, side == 'A' ? 'B' : 'A');

    if (_side_has_uid(mine, uid) || _side_has_uid(theirs, uid)) {
        printf("[GAME] UID already seated, ignoring repeat tap\n");
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS_PER_SIDE; i++) {
        if (!mine[i].filled) { slot = i; break; }
    }
    if (slot < 0) {
        printf("[GAME] Side %c already full\n", side);
        return;
    }

    mine[slot].filled = true;
    memcpy(mine[slot].uid, uid, 4);
    strncpy(mine[slot].name, name ? name : "Guest", NAME_LEN - 1);
    mine[slot].name[NAME_LEN - 1] = '\0';
    printf("[GAME] Side %c slot %d = %s\n", side, slot + 1, mine[slot].name);

    bool was_playing = (g->state == GAME_PLAYING);
    if (!was_playing && _side_ready(g->side_a) && _side_ready(g->side_b)) {
        _start_playing(g);
    }
}

void game_sync_roster_slot(GameData *g, char side, int slot, bool filled,
                           const char *name, const uint8_t uid[4]) {
    if (side != 'A' && side != 'B') return;
    if (slot < 1 || slot > MAX_PLAYERS_PER_SIDE) return;

    PlayerSlot *s = &_side_slots(g, side)[slot - 1];
    s->filled = filled;
    if (filled) {
        strncpy(s->name, name ? name : "Guest", NAME_LEN - 1);
        s->name[NAME_LEN - 1] = '\0';
        if (uid) memcpy(s->uid, uid, 4);
    } else {
        s->name[0] = '\0';
        memset(s->uid, 0, 4);
    }
}

void game_recheck_start(GameData *g) {
    if (g->state == GAME_WAITING && _side_ready(g->side_a) && _side_ready(g->side_b)) {
        _start_playing(g);
    }
}

void game_update(GameData *g, const BallData *ball) {
    if (g->state != GAME_PLAYING) return;
    if (!ball->valid) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Grace period after game start
    if (now - g->game_start_ms < 400) return;

    // Lock baseline on first packet
    if (!g->_score_initialized) {
        g->_last_ball_score_a = ball->score_a;
        g->_last_ball_score_b = ball->score_b;
        g->_score_initialized = true;
        printf("[GAME] Baseline: sim=%d-%d\n",
               ball->score_a, ball->score_b);
        return;
    }

    // Don't detect goals during animation
    if (game_goal_animating(g)) {
        // Still update position/speed but skip goal detection
        g->current_kmh = ball->speed;
        g->possession  = ball->possession;
        g->ball_x = ball->x; g->ball_y = ball->y;
        g->field_w = ball->field_w; g->field_h = ball->field_h;
        // Keep baseline in sync so we don't detect stale goals after animation
        g->_last_ball_score_a = ball->score_a;
        g->_last_ball_score_b = ball->score_b;
        return;
    }

    // Detect goals
    if (ball->score_a != g->_last_ball_score_a) {
        g->score_a++;
        g->_last_ball_score_a = ball->score_a;
        g->goal_scorer      = 1;
        g->goal_anim_end_ms = now + 2500;
        printf("[GAME] GOAL A! %d-%d\n", g->score_a, g->score_b);
    }
    if (ball->score_b != g->_last_ball_score_b) {
        g->score_b++;
        g->_last_ball_score_b = ball->score_b;
        g->goal_scorer      = 2;
        g->goal_anim_end_ms = now + 2500;
        printf("[GAME] GOAL B! %d-%d\n", g->score_a, g->score_b);
    }

    // Update live data
    g->current_kmh = ball->speed;
    g->possession  = ball->possession;
    g->ball_x = ball->x; g->ball_y = ball->y;
    g->field_w = ball->field_w; g->field_h = ball->field_h;
    if (ball->speed > g->fastest_kmh && ball->speed < 200.0f)
        g->fastest_kmh = ball->speed;

    // Win check
    if (g->score_a >= MAX_SCORE) {
        g->state = GAME_OVER; g->winner = 1;
        g->game_end_ms = now;
        printf("[GAME] Side A wins!\n");
    } else if (g->score_b >= MAX_SCORE) {
        g->state = GAME_OVER; g->winner = 2;
        g->game_end_ms = now;
        printf("[GAME] Side B wins!\n");
    }
}

bool game_goal_animating(const GameData *g) {
    if (g->goal_anim_end_ms == 0) return false;
    return to_ms_since_boot(get_absolute_time()) < g->goal_anim_end_ms;
}

uint32_t game_elapsed_seconds(const GameData *g) {
    if (g->game_start_ms == 0) return 0;
    uint32_t end = (g->state == GAME_OVER) ? g->game_end_ms
                                            : to_ms_since_boot(get_absolute_time());
    return (end - g->game_start_ms) / 1000;
}

const char* game_state_label(GameState s) {
    switch (s) {
        case GAME_WAITING: return "WAITING";
        case GAME_PLAYING: return "PLAYING";
        case GAME_OVER:    return "GAME_OVER";
        default: return "?";
    }
}

void game_side_label(const GameData *g, char side, char *out, size_t out_sz) {
    const PlayerSlot *slots = (side == 'A') ? g->side_a : g->side_b;
    if (!slots[0].filled) {
        snprintf(out, out_sz, "---");
    } else if (slots[1].filled) {
        snprintf(out, out_sz, "%s+%s", slots[0].name, slots[1].name);
    } else {
        snprintf(out, out_sz, "%s", slots[0].name);
    }
}
