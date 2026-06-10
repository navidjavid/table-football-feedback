#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/spi.h"

// ============================================================
// PN532 SPI wiring
// ============================================================
#define PN532_SPI       spi1

#define PN532_PIN_SCK   10
#define PN532_PIN_MOSI  11
#define PN532_PIN_MISO  12
#define PN532_PIN_CS    13

#define PN532_USE_RESET 0
#define PN532_PIN_RST   15

// ============================================================
// PN532 constants
// ============================================================
#define PN532_SPI_DATAWRITE  0x01
#define PN532_SPI_STATREAD   0x02
#define PN532_SPI_DATAREAD   0x03
#define PN532_SPI_READY      0x01

#define PN532_PREAMBLE       0x00
#define PN532_STARTCODE1     0x00
#define PN532_STARTCODE2     0xFF
#define PN532_POSTAMBLE      0x00

#define PN532_HOSTTOPN532    0xD4
#define PN532_PN532TOHOST    0xD5

#define PN532_COMMAND_GETFIRMWAREVERSION  0x02
#define PN532_COMMAND_SAMCONFIGURATION    0x14
#define PN532_COMMAND_INLISTPASSIVETARGET 0x4A

// ============================================================
// Low-level SPI helpers (WITH BIT REVERSAL FIX)
// ============================================================

// The Pico hardware SPI does not support LSB_FIRST.
// We MUST reverse the bits in software for the PN532!
static inline uint8_t reverse_bit(uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

static void pn532_select(void) {
    gpio_put(PN532_PIN_CS, 0);
    sleep_ms(2); // PN532 requires 2ms to wake up upon CS going low!
}

static void pn532_deselect(void) {
    gpio_put(PN532_PIN_CS, 1);
    sleep_ms(1); 
}

static void pn532_spi_write_byte(uint8_t value) {
    uint8_t reversed = reverse_bit(value);
    spi_write_blocking(PN532_SPI, &reversed, 1);
}

static uint8_t pn532_spi_read_byte(void) {
    uint8_t value = 0x00;
    // Sending 0x00 to push the clock (reversed 0x00 is still 0x00)
    spi_read_blocking(PN532_SPI, 0x00, &value, 1);
    return reverse_bit(value);
}

static void pn532_spi_read_bytes(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] = pn532_spi_read_byte();
    }
}

// ============================================================
// PN532 SPI protocol
// ============================================================
static uint8_t pn532_read_status(void) {
    uint8_t status;
    pn532_select();
    pn532_spi_write_byte(PN532_SPI_STATREAD);
    status = pn532_spi_read_byte();
    pn532_deselect();
    return status;
}

static bool pn532_wait_ready(uint32_t timeout_ms) {
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    while (!time_reached(timeout)) {
        if (pn532_read_status() == PN532_SPI_READY) {
            return true;
        }
        sleep_ms(5);
    }
    return false;
}

static void pn532_read_data(uint8_t *buffer, size_t len) {
    pn532_select();
    pn532_spi_write_byte(PN532_SPI_DATAREAD);
    pn532_spi_read_bytes(buffer, len);
    pn532_deselect();
}

static void pn532_write_frame(uint8_t command, const uint8_t *params, uint8_t params_len) {
    uint8_t len = params_len + 2; 
    uint8_t lcs = (uint8_t)(~len + 1);
    uint8_t sum = 0;

    pn532_select();

    pn532_spi_write_byte(PN532_SPI_DATAWRITE);
    pn532_spi_write_byte(PN532_PREAMBLE);
    pn532_spi_write_byte(PN532_STARTCODE1);
    pn532_spi_write_byte(PN532_STARTCODE2);
    pn532_spi_write_byte(len);
    pn532_spi_write_byte(lcs);

    pn532_spi_write_byte(PN532_HOSTTOPN532);
    sum += PN532_HOSTTOPN532;

    pn532_spi_write_byte(command);
    sum += command;

    for (uint8_t i = 0; i < params_len; i++) {
        pn532_spi_write_byte(params[i]);
        sum += params[i];
    }

    uint8_t dcs = (uint8_t)(~sum + 1);
    pn532_spi_write_byte(dcs);
    pn532_spi_write_byte(PN532_POSTAMBLE);

    pn532_deselect();
}

static bool pn532_read_ack(void) {
    uint8_t ack_buf[16]; // Oversized to handle dummy bytes
    
    if (!pn532_wait_ready(1000)) return false;

    pn532_read_data(ack_buf, sizeof(ack_buf));

    // Slide window to find 0x00 0x00 0xFF (ignores leading garbage byte)
    for (int i = 0; i < sizeof(ack_buf) - 6; i++) {
        if (ack_buf[i] == 0x00 && ack_buf[i+1] == 0x00 && ack_buf[i+2] == 0xFF) {
            // Check the rest of the ACK frame
            if (ack_buf[i+3] == 0x00 && ack_buf[i+4] == 0xFF && ack_buf[i+5] == 0x00) {
                return true;
            }
        }
    }
    printf("[PN532] Invalid ACK frame\n");
    return false;
}

static bool pn532_send_command(uint8_t command, const uint8_t *params, uint8_t params_len) {
    pn532_write_frame(command, params, params_len);
    return pn532_read_ack();
}

static bool pn532_read_response(uint8_t expected_response, uint8_t *payload, uint8_t *payload_len, uint32_t timeout_ms) {
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));

    if (!pn532_wait_ready(timeout_ms)) return false;

    pn532_read_data(frame, sizeof(frame));

    // Search for frame start to ignore leading SPI garbage
    int offset = -1;
    for (int i = 0; i < sizeof(frame) - 6; i++) {
        if (frame[i] == 0x00 && frame[i+1] == 0x00 && frame[i+2] == 0xFF) {
            offset = i;
            break;
        }
    }

    if (offset < 0) {
        printf("[PN532] Bad response preamble\n");
        return false;
    }

    uint8_t len = frame[offset + 3];
    uint8_t lcs = frame[offset + 4];

    if ((uint8_t)(len + lcs) != 0x00) return false;

    uint8_t sum = 0;
    for (uint8_t i = 0; i < len; i++) {
        sum += frame[offset + 5 + i];
    }

    uint8_t dcs = frame[offset + 5 + len];
    if ((uint8_t)(sum + dcs) != 0x00) return false;

    if (frame[offset + 5] != PN532_PN532TOHOST) return false;
    if (frame[offset + 6] != expected_response) return false;

    uint8_t real_payload_len = len - 2;

    if (payload && payload_len) {
        if (*payload_len < real_payload_len) return false;
        memcpy(payload, &frame[offset + 7], real_payload_len);
        *payload_len = real_payload_len;
    }

    return true;
}

// ============================================================
// High-level PN532 commands
// ============================================================
static bool pn532_get_firmware_version(void) {
    uint8_t payload[16];
    uint8_t payload_len = sizeof(payload);

    printf("[PN532] Sending GetFirmwareVersion...\n");

    if (!pn532_send_command(PN532_COMMAND_GETFIRMWAREVERSION, NULL, 0)) return false;
    if (!pn532_read_response(PN532_COMMAND_GETFIRMWAREVERSION + 1, payload, &payload_len, 1000)) return false;

    printf("[PN532] Firmware OK. IC=0x%02X Ver=0x%02X Rev=0x%02X Support=0x%02X\n",
           payload[0], payload[1], payload[2], payload[3]);
    return true;
}

static bool pn532_sam_config(void) {
    uint8_t params[] = { 0x01, 0x14, 0x01 };
    uint8_t payload[8];
    uint8_t payload_len = sizeof(payload);

    printf("[PN532] Sending SAMConfiguration...\n");
    if (!pn532_send_command(PN532_COMMAND_SAMCONFIGURATION, params, sizeof(params))) return false;
    if (!pn532_read_response(PN532_COMMAND_SAMCONFIGURATION + 1, payload, &payload_len, 1000)) return false;

    printf("[PN532] SAMConfiguration OK\n");
    return true;
}

static bool pn532_read_passive_target(uint8_t *uid, uint8_t *uid_len) {
    uint8_t params[] = { 0x01, 0x00 }; // 1 target, ISO14443A
    uint8_t payload[32];
    uint8_t payload_len = sizeof(payload);

    if (!pn532_send_command(PN532_COMMAND_INLISTPASSIVETARGET, params, sizeof(params))) return false;
    if (!pn532_read_response(PN532_COMMAND_INLISTPASSIVETARGET + 1, payload, &payload_len, 1000)) return false;

    uint8_t nfcid_len = payload[5];
    if (nfcid_len == 0 || nfcid_len > 10) return false;

    memcpy(uid, &payload[6], nfcid_len);
    *uid_len = nfcid_len;

    return true;
}

// ============================================================
// Hardware init
// ============================================================
static void pn532_hw_init(void) {
    printf("[PN532] Initialising SPI1 pins...\n");

    spi_init(PN532_SPI, 500 * 1000);

    // MUST use MSB first - we reverse in software to fix PL022 limit!
    spi_set_format(PN532_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PN532_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PN532_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PN532_PIN_MISO, GPIO_FUNC_SPI);

    gpio_init(PN532_PIN_CS);
    gpio_set_dir(PN532_PIN_CS, GPIO_OUT);
    gpio_put(PN532_PIN_CS, 1);

#if PN532_USE_RESET
    gpio_init(PN532_PIN_RST);
    gpio_set_dir(PN532_PIN_RST, GPIO_OUT);
    gpio_put(PN532_PIN_RST, 0);
    sleep_ms(100);
    gpio_put(PN532_PIN_RST, 1);
    sleep_ms(500);
#else
    sleep_ms(1000);
#endif
}

// ============================================================
// Main
// ============================================================
int main(void) {
    stdio_init_all();
    while (!stdio_usb_connected()) { sleep_ms(100); }

    printf("\n========================================\n");
    printf("PN532 DIRECT SPI TEST (Hardware Bit Reversal Fix)\n");
    printf("========================================\n");

    pn532_hw_init();

    if (!pn532_get_firmware_version()) {
        printf("\nPN532 firmware read failed.\n");
        while (true) {
            printf("[PN532] Status byte = 0x%02X\n", pn532_read_status());
            sleep_ms(1000);
        }
    }

    if (!pn532_sam_config()) {
        while (true) sleep_ms(1000);
    }

    printf("\nPN532 ready. Tap a card/tag on the antenna.\n");

    uint8_t uid[10];
    uint8_t uid_len;
    uint32_t counter = 0;

    while (true) {
        uid_len = 0;
        memset(uid, 0, sizeof(uid));

        if (pn532_read_passive_target(uid, &uid_len)) {
            printf("[CARD] UID length=%u UID=", uid_len);
            for (uint8_t i = 0; i < uid_len; i++) {
                printf("%02X", uid[i]);
                if (i + 1 < uid_len) printf(":");
            }
            printf("\n");
            sleep_ms(1000);
        } else {
            if ((counter % 5) == 0) {
                printf("[PN532] Waiting for card...\n");
            }
        }
        counter++;
        sleep_ms(200);
    }
    return 0;
}