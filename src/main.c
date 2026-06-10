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

    rfid_handler_init();    printf("[init] RFID\n");
    i2c_comms_init();       printf("[init] I2C slave\n");
    display_manager_init(); printf("[init] Display\n");

    display_manager_show_splash();
    sleep_ms(1500);

    GameData  game;
    game_init(&game);

    BallData  ball  = {0};
    BallData  fresh = {0};
    GameState prev_state = GAME_REGISTER_P1;
    uint32_t  loop  = 0;

    while (true) {
        loop++;

        // Detect state transitions
        if (game.state != prev_state) {
            printf("[STATE] %s → %s\n",
                   game_state_label(prev_state),
                   game_state_label(game.state));

            // Flush stale I2C bytes when game starts
            if (game.state == GAME_PLAYING) {
                i2c_comms_flush();
                printf("[I2C] Ring buffer flushed for new game\n");
            }
            prev_state = game.state;
        }

        // Poll I2C
        i2c_comms_poll(&fresh);
        if (fresh.valid) {
            ball = fresh;
            game_update(&game, &ball);
        }

        // Poll RFID
        RFIDScanResult rfid;
        rfid_handler_scan(&rfid);
        if (rfid.result == RFID_NEW_TAP) {
            const char *name = game_lookup_player(rfid.uid);
            printf("[RFID] %02X:%02X:%02X:%02X → %s\n",
                   rfid.uid[0], rfid.uid[1], rfid.uid[2], rfid.uid[3],
                   name ? name : "Guest");
            game_register_player(&game, rfid.uid, name);
        }

        // Render every ~250ms
        if (loop % 3 == 0)
            display_manager_render(&game);

        sleep_ms(80);
    }
}
