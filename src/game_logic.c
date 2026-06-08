#include "game_logic.h"
#include "pico/stdlib.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Player registry — known UIDs mapped to names
// Replace these with YOUR actual card UIDs
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t     uid[4];
    const char *name;
} KnownPlayer;

static const KnownPlayer _known[] = {
    { { 0xDB, 0xEF, 0x70, 0x05 }, "Alice" },
    { { 0x00, 0x00, 0x00, 0x01 }, "Bob"   },   // replace with real UID
    { { 0x00, 0x00, 0x00, 0x02 }, "Carol" },
    { { 0x00, 0x00, 0x00, 0x03 }, "Dave"  },
};
static const int _known_count = sizeof(_known) / sizeof(_known[0]);

const char* game_lookup_player(const uint8_t uid[4]) {
    for (int i = 0; i < _known_count; i++) {
        if (memcmp(_known[i].uid, uid, 4) == 0)
            return _known[i].name;
    }
    return NULL;
}

void game_init(GameData *g) {
    memset(g, 0, sizeof(*g));
    g->state   = GAME_REGISTER_P1;
    strcpy(g->p1_name, "---");
    strcpy(g->p2_name, "---");
}

void game_register_player(GameData *g, const uint8_t uid[4], const char *name) {
    if (g->state == GAME_REGISTER_P1) {
        memcpy(g->p1_uid, uid, 4);
        strncpy(g->p1_name, name ? name : "Guest", NAME_LEN - 1);
        g->p1_name[NAME_LEN - 1] = '\0';
        g->state = GAME_REGISTER_P2;
    }
    else if (g->state == GAME_REGISTER_P2) {
        // Don't allow same card as P1
        if (memcmp(uid, g->p1_uid, 4) == 0) return;

        memcpy(g->p2_uid, uid, 4);
        strncpy(g->p2_name, name ? name : "Guest", NAME_LEN - 1);
        g->p2_name[NAME_LEN - 1] = '\0';
        g->state = GAME_PLAYING;
        g->game_start_ms = to_ms_since_boot(get_absolute_time());
        g->score_a = 0;
        g->score_b = 0;
        g->fastest_kmh = 0;
    }
    else if (g->state == GAME_OVER) {
        // Start new game
        game_init(g);
    }
}

void game_update(GameData *g, const BallData *ball) {
    if (g->state != GAME_PLAYING) return;
    if (!ball->valid) return;

    g->score_a    = ball->score_a;
    g->score_b    = ball->score_b;
    g->current_kmh= ball->speed;
    g->possession = ball->possession;
    g->ball_x     = ball->x;
    g->ball_y     = ball->y;
    g->field_w    = ball->field_w;
    g->field_h    = ball->field_h;

    if (ball->speed > g->fastest_kmh && ball->speed < 200.0f)
        g->fastest_kmh = ball->speed;

    if (g->score_a >= MAX_SCORE) {
        g->state    = GAME_OVER;
        g->winner   = 1;
        g->game_end_ms = to_ms_since_boot(get_absolute_time());
    }
    if (g->score_b >= MAX_SCORE) {
        g->state    = GAME_OVER;
        g->winner   = 2;
        g->game_end_ms = to_ms_since_boot(get_absolute_time());
    }
}

uint32_t game_elapsed_seconds(const GameData *g) {
    uint32_t end = (g->state == GAME_OVER) ? g->game_end_ms
                                            : to_ms_since_boot(get_absolute_time());
    if (g->game_start_ms == 0) return 0;
    return (end - g->game_start_ms) / 1000;
}

const char* game_state_label(GameState s) {
    switch (s) {
        case GAME_REGISTER_P1: return "REGISTER_P1";
        case GAME_REGISTER_P2: return "REGISTER_P2";
        case GAME_PLAYING:     return "PLAYING";
        case GAME_OVER:        return "GAME_OVER";
        default:               return "?";
    }
}
