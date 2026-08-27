// Standalone test using the ACTUAL production driver (lib/pn532/pn532.c),
// the one that reportedly worked on breadboard last month — not a
// rewritten/experimental version. Same pins and config as rfid_handler.c.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "pn532.h"

#define RFID_SCK  10
#define RFID_MOSI 11
#define RFID_MISO 12
#define RFID_CS   13
#define RFID_RST  15

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n========================================\n");
    printf("PN532 LIBRARY TEST (lib/pn532/pn532.c, production driver)\n");
    printf("========================================\n");
    printf("SPI1  SCK=GP%d  MOSI=GP%d  MISO=GP%d  CS=GP%d  RST=GP%d\n",
           RFID_SCK, RFID_MOSI, RFID_MISO, RFID_CS, RFID_RST);

    PN532Config cfg = {
        .spi = spi1, .pin_sck = RFID_SCK, .pin_mosi = RFID_MOSI,
        .pin_miso = RFID_MISO, .pin_cs = RFID_CS, .pin_rst = RFID_RST,
    };
    pn532_init(&cfg); // prints its own diagnostics (see pn532.c)

    printf("\nPlace an NFC/RFID card on the reader.\n");

    uint8_t uid[4];
    while (true) {
        int status = pn532_read_card(uid);
        if (status == PN532_OK) {
            printf("[CARD] UID=%02X:%02X:%02X:%02X\n", uid[0], uid[1], uid[2], uid[3]);
            pn532_halt();
            sleep_ms(1000);
        }
        sleep_ms(200);
    }
    return 0;
}
