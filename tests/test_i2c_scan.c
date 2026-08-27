// Minimal I2C bus scanner — protocol-independent PN532 aliveness test.
//
// Uses the Pico's plain hardware I2C peripheral (no custom bit manipulation),
// so a response here proves the chip itself is alive regardless of whether
// our SPI framing/bit-reversal code has a bug.
//
// Wiring (standalone jumper test, separate from the main SPI wiring):
//   PN532 SDA -> Pico GP4  (physical pin 6)
//   PN532 SCL -> Pico GP5  (physical pin 7)
//   PN532 VCC -> Pico 3V3  (physical pin 36)
//   PN532 GND -> Pico GND
//
// Before running: flip the PN532's DIP switch to I2C mode (try each
// combination you haven't already tested for SPI) and fully power-cycle
// (unplug USB, wait ~10s, replug) so the mode is freshly latched.
//
// PN532's default I2C address is 0x24 (7-bit).

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/i2c.h"

#define SCAN_I2C   i2c0
#define PIN_SDA    4
#define PIN_SCL    5

int main(void) {
    stdio_init_all();
    while (!stdio_usb_connected()) { sleep_ms(100); }

    printf("\n========================================\n");
    printf("I2C BUS SCANNER (PN532 aliveness test)\n");
    printf("========================================\n");
    printf("SDA=GP%d  SCL=GP%d\n", PIN_SDA, PIN_SCL);

    i2c_init(SCAN_I2C, 100 * 1000); // 100kHz, safe/standard speed
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    sleep_ms(500);

    while (true) {
        printf("\nScanning 0x08..0x77 ...\n");
        int found = 0;

        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            uint8_t dummy;
            // A 0-byte write; ACK on the address byte alone is enough to
            // prove something answered, without needing a real command.
            int ret = i2c_read_blocking(SCAN_I2C, addr, &dummy, 1, false);

            if (ret >= 0) {
                found++;
                if (addr == 0x24) {
                    printf("  0x%02X  <-- responded! (PN532 default I2C address)\n", addr);
                } else {
                    printf("  0x%02X  <-- responded\n", addr);
                }
            }
        }

        if (found == 0) {
            printf("No devices found on the bus.\n");
        } else {
            printf("Scan complete: %d device(s) responded.\n", found);
        }

        sleep_ms(3000);
    }
    return 0;
}
