#include "display_manager.h"
#include "ea_dogl128.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// Display pins
#define DISP_SCK   18
#define DISP_MOSI  19
#define DISP_CS    17
#define DISP_A0    20
#define DISP_RST   21

void display_manager_init(void) {
    DOGL128Config cfg = {
        .pin_sck  = DISP_SCK,
        .pin_mosi = DISP_MOSI,
        .pin_cs   = DISP_CS,
        .pin_a0   = DISP_A0,
        .pin_rst  = DISP_RST,
        .contrast = 0x13,
    };
    dogl128_init(&cfg);
}

void display_manager_show_splash(void) {
    dogl128_clear();
    dogl128_rect(0, 0, 128, 64);
    dogl128_rect(2, 2, 124, 60);

    dogl128_draw_string_2x(10, 12, "FOOSBALL");
    dogl128_draw_string(22, 32, "Feedback System");

    dogl128_hline(20, 48, 88);
    dogl128_draw_string(28, 53, "Starting...");
    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Screen: Register Player 1
// ---------------------------------------------------------------------------
static void render_register_p1(const GameData *g) {
    (void)g;
    dogl128_clear();

    // Title bar
    dogl128_fill_rect(0, 0, 128, 11);
    dogl128_invert_rect(0, 0, 128, 11);
    dogl128_draw_string(8, 2, "REGISTER PLAYERS");
    dogl128_invert_rect(0, 0, 128, 11);

    // Player slots
    dogl128_draw_string(6, 18, "Player 1:");
    dogl128_draw_string(70, 18, "----");
    dogl128_draw_string(6, 30, "Player 2:");
    dogl128_draw_string(70, 30, "----");

    // Prompt with blinking arrow
    dogl128_rect(0, 44, 128, 20);
    static int blink = 0;
    blink++;
    if (blink % 2 == 0)
        dogl128_draw_string(8, 50, ">> Tap card for P1");
    else
        dogl128_draw_string(8, 50, "   Tap card for P1");

    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Screen: Register Player 2
// ---------------------------------------------------------------------------
static void render_register_p2(const GameData *g) {
    dogl128_clear();

    dogl128_fill_rect(0, 0, 128, 11);
    dogl128_invert_rect(0, 0, 128, 11);
    dogl128_draw_string(8, 2, "REGISTER PLAYERS");
    dogl128_invert_rect(0, 0, 128, 11);

    char line[24];
    snprintf(line, sizeof(line), "P1: %s", g->p1_name);
    dogl128_draw_string(6, 18, line);

    // Checkmark next to P1
    dogl128_draw_string(110, 18, "OK");

    dogl128_draw_string(6, 30, "P2: ----");

    dogl128_rect(0, 44, 128, 20);
    static int blink = 0;
    blink++;
    if (blink % 2 == 0)
        dogl128_draw_string(8, 50, ">> Tap card for P2");
    else
        dogl128_draw_string(8, 50, "   Tap card for P2");

    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Screen: Playing — live game
//
// Layout:
//  [Title bar with names                                ]
//  [                                                    ]
//  [   P1  3   :   5  P2                                ]   big score
//  [                                                    ]
//  [field rect with ball indicator                      ]
//  [Speed   Fastest    Time   Poss                      ]
// ---------------------------------------------------------------------------
static void render_playing(const GameData *g) {
    dogl128_clear();

    // === Title bar: player names ===
    dogl128_fill_rect(0, 0, 128, 9);
    dogl128_invert_rect(0, 0, 128, 9);
    char nameline[24];
    snprintf(nameline, sizeof(nameline), "%-8s     %8s", g->p1_name, g->p2_name);
    dogl128_draw_string(2, 1, nameline);
    dogl128_invert_rect(0, 0, 128, 9);

    // === Big score ===
    char sa[2] = { '0' + (g->score_a % 10), 0 };
    char sb[2] = { '0' + (g->score_b % 10), 0 };
    dogl128_draw_char_2x(28, 13, sa[0]);
    dogl128_draw_string_2x(50, 13, "-");
    dogl128_draw_char_2x(80, 13, sb[0]);

    // === Mini field 90x12 at (19, 32) ===
    int fx = 19, fy = 32, fw = 90, fh = 12;
    dogl128_rect(fx, fy, fw, fh);
    dogl128_vline(fx + fw/2, fy, fh);   // halfway line

    // Ball position (map field 1000x500 to fw x fh)
    if (g->field_w > 0 && g->field_h > 0) {
        int bx = fx + (g->ball_x * (fw - 2)) / g->field_w + 1;
        int by = fy + (g->ball_y * (fh - 2)) / g->field_h + 1;
        if (bx < fx + 1) bx = fx + 1;
        if (bx > fx + fw - 2) bx = fx + fw - 2;
        if (by < fy + 1) by = fy + 1;
        if (by > fy + fh - 2) by = fy + fh - 2;
        // Draw 3x3 ball
        dogl128_fill_rect(bx - 1, by - 1, 3, 3);
    }

    // === Bottom stats ===
    char stat[32];
    snprintf(stat, sizeof(stat), "%4.1fkm/h Fast:%4.1f",
             g->current_kmh, g->fastest_kmh);
    dogl128_draw_string(0, 48, stat);

    uint32_t sec = game_elapsed_seconds(g);
    snprintf(stat, sizeof(stat), "T:%02lu:%02lu  Poss:%s",
             sec / 60, sec % 60,
             g->possession == 1 ? "A" :
             g->possession == 2 ? "B" : "-");
    dogl128_draw_string(0, 57, stat);

    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Screen: Game Over
// ---------------------------------------------------------------------------
static void render_game_over(const GameData *g) {
    dogl128_clear();
    dogl128_rect(0, 0, 128, 64);
    dogl128_rect(2, 2, 124, 60);

    dogl128_fill_rect(2, 2, 124, 11);
    dogl128_invert_rect(2, 2, 124, 11);
    dogl128_draw_string(36, 4, "GAME OVER");
    dogl128_invert_rect(2, 2, 124, 11);

    char line[24];
    const char *winner = (g->winner == 1) ? g->p1_name :
                          (g->winner == 2) ? g->p2_name : "DRAW";
    snprintf(line, sizeof(line), "Winner: %s", winner);
    dogl128_draw_string(8, 17, line);

    snprintf(line, sizeof(line), "Final: %d - %d", g->score_a, g->score_b);
    dogl128_draw_string(8, 27, line);

    snprintf(line, sizeof(line), "Fastest: %.1f km/h", g->fastest_kmh);
    dogl128_draw_string(8, 37, line);

    uint32_t sec = (g->game_end_ms - g->game_start_ms) / 1000;
    snprintf(line, sizeof(line), "Time: %02lu:%02lu", sec / 60, sec % 60);
    dogl128_draw_string(8, 47, line);

    static int blink = 0;
    blink++;
    if (blink % 2 == 0)
        dogl128_draw_string(8, 57, "Tap card for new game");

    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Public render dispatch
// ---------------------------------------------------------------------------
void display_manager_render(const GameData *g) {
    switch (g->state) {
        case GAME_REGISTER_P1: render_register_p1(g); break;
        case GAME_REGISTER_P2: render_register_p2(g); break;
        case GAME_PLAYING:     render_playing(g);     break;
        case GAME_OVER:        render_game_over(g);   break;
    }
}
