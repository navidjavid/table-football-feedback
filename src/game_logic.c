#include "game_logic.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Known players
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

static void _start_p1(GameData *g, const uint8_t uid[4], const char *name) {
    memcpy(g->p1_uid, uid, 4);
    strncpy(g->p1_name, name ? name : "Guest", NAME_LEN - 1);
    g->p1_name[NAME_LEN - 1] = '\0';
    g->state = GAME_REGISTER_P2;
    printf("[GAME] P1 = %s\n", g->p1_name);
}

static void _start_p2(GameData *g, const uint8_t uid[4], const char *name) {
    memcpy(g->p2_uid, uid, 4);
    strncpy(g->p2_name, name ? name : "Guest", NAME_LEN - 1);
    g->p2_name[NAME_LEN - 1] = '\0';
    g->state               = GAME_PLAYING;
    g->score_a             = 0;
    g->score_b             = 0;
    g->fastest_kmh         = 0;
    g->goal_anim_end_ms    = 0;
    g->goal_scorer         = 0;
    g->_score_initialized  = false;
    g->game_start_ms       = to_ms_since_boot(get_absolute_time());
    g->game_end_ms         = 0;
    g->winner              = 0;
    printf("[GAME] P2 = %s -> PLAYING\n", g->p2_name);
}

void game_init(GameData *g) {
    memset(g, 0, sizeof(*g));
    g->state = GAME_REGISTER_P1;
    strcpy(g->p1_name, "---");
    strcpy(g->p2_name, "---");
}

void game_register_player(GameData *g, const uint8_t uid[4],
                           const char *name) {
    switch (g->state) {
        case GAME_REGISTER_P1:
            _start_p1(g, uid, name);
            break;
        case GAME_REGISTER_P2:
            if (memcmp(uid, g->p1_uid, 4) == 0) {
                printf("[GAME] Same card as P1\n");
                return;
            }
            _start_p2(g, uid, name);
            break;
        case GAME_OVER:
            game_init(g);
            _start_p1(g, uid, name);
            break;
        default: break;
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
        printf("[GAME] %s wins!\n", g->p1_name);
    } else if (g->score_b >= MAX_SCORE) {
        g->state = GAME_OVER; g->winner = 2;
        g->game_end_ms = now;
        printf("[GAME] %s wins!\n", g->p2_name);
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
        case GAME_REGISTER_P1: return "REG_P1";
        case GAME_REGISTER_P2: return "REG_P2";
        case GAME_PLAYING:     return "PLAYING";
        case GAME_OVER:        return "GAME_OVER";
        default: return "?";
    }
}
