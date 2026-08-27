#include "display_manager.h"
#include "ea_dogl128.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

void display_manager_init(void) {
    DOGL128Config cfg = {
        .pin_sck = 18, .pin_mosi = 19, .pin_cs = 17,
        .pin_a0  = 20, .pin_rst  = 21, .contrast = 0x13,
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

void display_manager_show_status(const char *line) {
    dogl128_clear();
    dogl128_rect(0, 0, 128, 64);
    dogl128_rect(2, 2, 124, 60);
    dogl128_draw_string_2x(10, 12, "FOOSBALL");
    dogl128_draw_string(22, 32, "Feedback System");
    dogl128_hline(20, 48, 88);
    dogl128_draw_string(4, 53, line);
    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Shown before the game starts (WAITING) AND still shown as an overlay
// hint during PLAYING when a side only has one player — either state can
// accept a second tap on the still-open side(s) at any time.
static void render_waiting(const GameData *g) {
    dogl128_clear();
    dogl128_fill_rect(0, 0, 128, 11);
    dogl128_invert_rect(0, 0, 128, 11);
    dogl128_draw_string(14, 2, "REGISTER PLAYERS");
    dogl128_invert_rect(0, 0, 128, 11);

    char label[34], line[40];
    game_side_label(g, 'A', label, sizeof(label));
    snprintf(line, sizeof(line), "Side A: %s", label);
    dogl128_draw_string(4, 18, line);

    game_side_label(g, 'B', label, sizeof(label));
    snprintf(line, sizeof(line), "Side B: %s", label);
    dogl128_draw_string(4, 30, line);

    dogl128_rect(0, 44, 128, 20);
    static int b = 0; b++;
    if (b % 2 == 0)
        dogl128_draw_string(2, 50, "> Tap card to join <");
    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Goal animation: expanding rings + ball icon + score
// ---------------------------------------------------------------------------
static void render_goal(const GameData *g) {
    dogl128_clear();

    uint32_t now     = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - (g->goal_anim_end_ms - 2500); // ms since anim start

    // Phase 1 (0-600ms): expanding rectangles from centre
    if (elapsed < 600) {
        int step = elapsed / 75;  // 0..7
        for (int i = 0; i <= step; i++) {
            int margin = 32 - i * 4;
            if (margin < 0) margin = 0;
            dogl128_rect(margin, margin / 2,
                         128 - margin * 2, 64 - margin);
        }
        // Ball icon bouncing towards goal
        int ball_x = 64 + (int)(elapsed * 60 / 600) - 30;
        dogl128_fill_rect(ball_x, 28, 5, 5);
        dogl128_flush();
        return;
    }

    // Phase 2 (600ms-2500ms): GOAL! screen with pulsing border
    bool pulse = (elapsed / 250) % 2 == 0;

    if (pulse) {
        dogl128_rect(0, 0, 128, 64);
        dogl128_rect(1, 1, 126, 62);
    }
    dogl128_rect(3, 3, 122, 58);

    // GOAL! in big text
    dogl128_draw_string_2x(14, 6, "GOAL!");

    // Scorer
    char label[34];
    game_side_label(g, (g->goal_scorer == 1) ? 'A' : 'B', label, sizeof(label));
    char line[40];
    snprintf(line, sizeof(line), "%s scores!", label);
    int len = strlen(line);
    int x_center = (128 - len * 6) / 2;
    if (x_center < 0) x_center = 0;
    dogl128_draw_string(x_center, 26, line);

    // Score in 2x
    char sa[2] = { '0' + (g->score_a % 10), 0 };
    char sb[2] = { '0' + (g->score_b % 10), 0 };
    dogl128_draw_char_2x(32, 38, sa[0]);
    dogl128_draw_string(56, 42, ":");
    dogl128_draw_char_2x(68, 38, sb[0]);

    // Decorative lines that grow
    int bar_w = (elapsed - 600) * 50 / 1900;
    if (bar_w > 50) bar_w = 50;
    dogl128_hline(64 - bar_w, 56, bar_w * 2);
    dogl128_hline(64 - bar_w, 57, bar_w * 2);

    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Live game screen
// ---------------------------------------------------------------------------
static void render_playing(const GameData *g) {
    dogl128_clear();

    // Title bar
    dogl128_fill_rect(0, 0, 128, 9);
    dogl128_invert_rect(0, 0, 128, 9);
    char label_a[34], label_b[34], bar[42];
    game_side_label(g, 'A', label_a, sizeof(label_a));
    game_side_label(g, 'B', label_b, sizeof(label_b));
    snprintf(bar, sizeof(bar), "%-9.9s   %9.9s", label_a, label_b);
    dogl128_draw_string(2, 1, bar);
    dogl128_invert_rect(0, 0, 128, 9);

    // Big score
    char sa[2] = { '0' + (g->score_a % 10), 0 };
    char sb[2] = { '0' + (g->score_b % 10), 0 };
    dogl128_draw_char_2x(28, 12, sa[0]);
    dogl128_draw_string_2x(50, 12, "-");
    dogl128_draw_char_2x(80, 12, sb[0]);

    // Mini field 90x12
    int fx = 19, fy = 32, fw = 90, fh = 12;
    dogl128_rect(fx, fy, fw, fh);
    dogl128_vline(fx + fw/2, fy, fh);

    // Ball
    if (g->field_w > 0 && g->field_h > 0) {
        int bx = fx + 1 + (g->ball_x * (fw - 3)) / g->field_w;
        int by = fy + 1 + (g->ball_y * (fh - 3)) / g->field_h;
        if (bx < fx+1) bx = fx+1;
        if (bx > fx+fw-2) bx = fx+fw-2;
        if (by < fy+1) by = fy+1;
        if (by > fy+fh-2) by = fy+fh-2;
        dogl128_fill_rect(bx-1, by-1, 3, 3);
    }

    // Stats
    char stat[32];
    snprintf(stat, sizeof(stat), "%.1fkm/h  Max:%.1fkm/h",
             g->current_kmh, g->fastest_kmh);
    dogl128_draw_string(0, 48, stat);

    uint32_t sec = game_elapsed_seconds(g);
    snprintf(stat, sizeof(stat), "Time %02lu:%02lu   Poss:%s",
             sec/60, sec%60,
             g->possession==1 ? "A" : g->possession==2 ? "B" : "-");
    dogl128_draw_string(0, 57, stat);

    dogl128_flush();
}

// ---------------------------------------------------------------------------
// Game over
// ---------------------------------------------------------------------------
static void render_game_over(const GameData *g) {
    dogl128_clear();
    dogl128_rect(0, 0, 128, 64);
    dogl128_rect(2, 2, 124, 60);
    dogl128_fill_rect(2, 2, 124, 11);
    dogl128_invert_rect(2, 2, 124, 11);
    dogl128_draw_string(36, 4, "GAME OVER");
    dogl128_invert_rect(2, 2, 124, 11);

    char line[40], w[34];
    if (g->winner == 1) game_side_label(g, 'A', w, sizeof(w));
    else if (g->winner == 2) game_side_label(g, 'B', w, sizeof(w));
    else snprintf(w, sizeof(w), "DRAW");
    snprintf(line, sizeof(line), "Winner: %s", w);
    dogl128_draw_string(8, 17, line);
    snprintf(line, sizeof(line), "Final: %d - %d", g->score_a, g->score_b);
    dogl128_draw_string(8, 27, line);
    snprintf(line, sizeof(line), "Fastest: %.1f km/h", g->fastest_kmh);
    dogl128_draw_string(8, 37, line);
    uint32_t sec = (g->game_end_ms - g->game_start_ms) / 1000;
    snprintf(line, sizeof(line), "Time: %02lu:%02lu", sec/60, sec%60);
    dogl128_draw_string(8, 47, line);
    static int b = 0; b++;
    if (b % 2 == 0)
        dogl128_draw_string(4, 57, "Tap card: new game");
    dogl128_flush();
}

// ---------------------------------------------------------------------------
void display_manager_render(const GameData *g) {
    if (g->state == GAME_PLAYING && game_goal_animating(g)) {
        render_goal(g);
        return;
    }
    switch (g->state) {
        case GAME_WAITING: render_waiting(g);   break;
        case GAME_PLAYING: render_playing(g);   break;
        case GAME_OVER:    render_game_over(g); break;
    }
}
