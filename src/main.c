#include <stdio.h>
#include "pico/stdlib.h"
#include "rfid_handler.h"
#include "game_logic.h"
#include "display_manager.h"
#include "i2c_comms.h"

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("Table Football Feedback System\n");

    rfid_handler_init();
    // game_logic_init();
    // display_manager_init();
    // i2c_comms_init();

    while (true) {
        RFIDScanResult rfid;
        rfid_handler_scan(&rfid);

        if (rfid.result == RFID_RESULT_KNOWN) {
            printf("Player tapped: %s\n", rfid.player_name);
            // game_logic_set_player(rfid.player_index);
        } else if (rfid.result == RFID_RESULT_UNKNOWN) {
            printf("Unknown card: %02X:%02X:%02X:%02X\n",
                   rfid.uid[0], rfid.uid[1], rfid.uid[2], rfid.uid[3]);
        }

        // i2c_comms_poll();
        // game_logic_update();
        // display_manager_render();

        sleep_ms(200);
    }
}
