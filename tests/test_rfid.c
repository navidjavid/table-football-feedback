/**
 * test_rfid.c — standalone RFID hardware test
 *
 * Build this instead of main.c to verify the RC522 works in isolation.
 * In CMakeLists.txt, swap main.c for tests/test_rfid.c in add_executable().
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "rfid_handler.h"

// Two example players — replace with real UIDs from your cards
static const uint8_t PLAYER1_UID[4] = { 0xDB, 0xEF, 0x70, 0x05 };
static const uint8_t PLAYER2_UID[4] = { 0x00, 0x00, 0x00, 0x00 }; // fill in

int main(void) {
    stdio_init_all();
    sleep_ms(3000);

    printf("\n=== RFID Handler Test ===\n");

    rfid_handler_init();
    rfid_handler_register_player(PLAYER1_UID, "Alice");
    rfid_handler_register_player(PLAYER2_UID, "Bob");

    printf("Registered 2 players. Scan a card...\n");
    printf("---------------------------------------\n");

    uint32_t scan = 0;
    while (true) {
        scan++;
        RFIDScanResult result;
        rfid_handler_scan(&result);

        switch (result.result) {
            case RFID_RESULT_KNOWN:
                printf("[%lu] Known player: %s  (UID: %02X:%02X:%02X:%02X)\n",
                       scan,
                       result.player_name,
                       result.uid[0], result.uid[1],
                       result.uid[2], result.uid[3]);
                break;

            case RFID_RESULT_UNKNOWN:
                printf("[%lu] Unknown card  (UID: %02X:%02X:%02X:%02X)\n",
                       scan,
                       result.uid[0], result.uid[1],
                       result.uid[2], result.uid[3]);
                break;

            case RFID_RESULT_NONE:
            default:
                if (scan % 40 == 0)
                    printf("[%lu] Waiting...\n", scan);
                break;
        }

        sleep_ms(200);
    }
}
