/**
 * Table Football Feedback System — Main Application
 *
 * Pipeline:
 *   1. RFID detects player taps → register Player 1 then Player 2
 *   2. I2C receives ball data from tracker board → game state updates
 *   3. Display shows current screen based on game state
 *
 * Init order matters: Display LAST (most sensitive to IRQ timing).
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "rfid_handler.h"
#include "i2c_comms.h"
#include "game_logic.h"
#include "display_manager.h"

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== Table Football Feedback ===\n");

    // Init order: peripherals first, display LAST
    rfid_handler_init();    printf("[init] RFID ready\n");
    i2c_comms_init();       printf("[init] I2C slave ready (0x42)\n");
    display_manager_init(); printf("[init] Display ready\n");

    display_manager_show_splash();
    sleep_ms(1500);

    GameData game;
    game_init(&game);

    BallData ball  = {0};
    BallData fresh = {0};
    uint32_t loop  = 0;
    uint32_t last_log = 0;

    while (true) {
        loop++;

        // --- Poll I2C ---
        i2c_comms_poll(&fresh);
        if (fresh.valid) {
            ball = fresh;
            game_update(&game, &ball);
        }

        // --- Poll RFID ---
        RFIDScanResult rfid;
        rfid_handler_scan(&rfid);
        if (rfid.result == RFID_NEW_TAP) {
            const char *name = game_lookup_player(rfid.uid);
            printf("[RFID] UID %02X:%02X:%02X:%02X => %s\n",
                   rfid.uid[0], rfid.uid[1], rfid.uid[2], rfid.uid[3],
                   name ? name : "Unknown");

            game_register_player(&game, rfid.uid,
                                 name ? name : "Guest");
            printf("[GAME] state -> %s\n", game_state_label(game.state));
        }

        // --- Render display every 250ms ---
        if (loop % 3 == 0) {
            display_manager_render(&game);
        }

        // --- Status log once a second ---
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_log > 1000) {
            last_log = now;
            if (game.state == GAME_PLAYING) {
                printf("[GAME] %s %d-%d %s  ball=(%d,%d) spd=%.1f\n",
                       game.p1_name, game.score_a,
                       game.score_b, game.p2_name,
                       game.ball_x, game.ball_y, game.current_kmh);
            }
        }

        sleep_ms(80);
    }
}
