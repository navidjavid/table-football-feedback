#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "i2c_comms.h"

#define MAX_SCORE  5
#define NAME_LEN   16

typedef enum {
    GAME_REGISTER_P1,
    GAME_REGISTER_P2,
    GAME_PLAYING,
    GAME_OVER,
} GameState;

typedef struct {
    GameState state;

    char    p1_name[NAME_LEN];
    char    p2_name[NAME_LEN];
    uint8_t p1_uid[4];
    uint8_t p2_uid[4];

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
    int      winner;   // 0=none 1=P1 2=P2
} GameData;

const char* game_lookup_player(const uint8_t uid[4]);
void        game_init(GameData *g);
void        game_register_player(GameData *g, const uint8_t uid[4],
                                  const char *name);
void        game_update(GameData *g, const BallData *ball);
bool        game_goal_animating(const GameData *g);
const char* game_state_label(GameState s);
uint32_t    game_elapsed_seconds(const GameData *g);
