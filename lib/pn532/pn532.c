/**
 * pn532.c — PN532 NFC/RFID driver for Raspberry Pi Pico
 *
 * SPI interface with LSB-first bit order (handled via software reversal).
 * Provides the same API as the MFRC522 driver for drop-in replacement.
 */

#include "pn532.h"
#include <string.h>
#include <stdio.h>

// SPI direction bytes
#define SPI_DATA_WRITE   0x01
#define SPI_STATUS_READ  0x02
#define SPI_DATA_READ    0x03

// PN532 TFI values
#define TFI_HOST_TO_PN   0xD4
#define TFI_PN_TO_HOST   0xD5

// PN532 commands
#define CMD_GET_FIRMWARE_VERSION  0x02
#define CMD_SAM_CONFIGURATION     0x14
#define CMD_RF_CONFIGURATION      0x32
#define CMD_IN_LIST_PASSIVE       0x4A
#define CMD_IN_RELEASE            0x52

static PN532Config _cfg;

// ---------------------------------------------------------------------------
// PN532 SPI is LSB-first. Pico SPI is MSB-first only.
// Reverse every byte before send / after receive.
// ---------------------------------------------------------------------------
static uint8_t _rev(uint8_t b) {
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

static void _cs_low(void)  { gpio_put(_cfg.pin_cs, 0); sleep_us(5); }
static void _cs_high(void) { sleep_us(5); gpio_put(_cfg.pin_cs, 1); sleep_us(5); }

static void _write_byte(uint8_t b) {
    uint8_t r = _rev(b);
    spi_write_blocking(_cfg.spi, &r, 1);
}

static uint8_t _read_byte(void) {
    uint8_t raw;
    spi_read_blocking(_cfg.spi, 0x00, &raw, 1);
    return _rev(raw);
}

// ---------------------------------------------------------------------------
// Frame write: preamble + start + len + lcs + data + dcs + postamble
// data[] starts with TFI (0xD4) then command byte then params
// ---------------------------------------------------------------------------
static void _write_frame(const uint8_t *data, uint8_t len) {
    _cs_low();
    _write_byte(SPI_DATA_WRITE);

    _write_byte(0x00);           // preamble
    _write_byte(0x00);           // start code byte 1
    _write_byte(0xFF);           // start code byte 2
    _write_byte(len);            // length
    _write_byte((uint8_t)(~len + 1)); // LCS (two's complement)

    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        _write_byte(data[i]);
        sum += data[i];
    }

    _write_byte((uint8_t)(~sum + 1)); // DCS
    _write_byte(0x00);                 // postamble

    _cs_high();
}

// ---------------------------------------------------------------------------
// Status poll — PN532 sets status byte to 0x01 when ready
// ---------------------------------------------------------------------------
static bool _is_ready(void) {
    _cs_low();
    _write_byte(SPI_STATUS_READ);
    uint8_t status = _read_byte();
    _cs_high();
    return (status == 0x01);
}

static bool _wait_ready(int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        if (_is_ready()) return true;
        sleep_ms(1);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Read ACK frame: 00 00 FF 00 FF 00
// ---------------------------------------------------------------------------
static bool _read_ack(void) {
    if (!_wait_ready(100)) return false;

    _cs_low();
    _write_byte(SPI_DATA_READ);

    uint8_t ack[6];
    for (int i = 0; i < 6; i++)
        ack[i] = _read_byte();

    _cs_high();

    return (ack[0] == 0x00 && ack[1] == 0x00 && ack[2] == 0xFF &&
            ack[3] == 0x00 && ack[4] == 0xFF && ack[5] == 0x00);
}

// ---------------------------------------------------------------------------
// Read response frame. Returns data length, fills buf with payload.
// buf[0] = TFI (0xD5), buf[1] = response code, buf[2..] = data
// ---------------------------------------------------------------------------
static int _read_response(uint8_t *buf, int buf_size) {
    _cs_low();
    _write_byte(SPI_DATA_READ);

    // Scan for start code: 00 FF
    uint8_t prev = 0xFF;
    bool found = false;
    for (int i = 0; i < 25; i++) {
        uint8_t b = _read_byte();
        if (prev == 0x00 && b == 0xFF) { found = true; break; }
        prev = b;
    }
    if (!found) { _cs_high(); return -1; }

    // Read LEN and LCS
    uint8_t len = _read_byte();
    uint8_t lcs = _read_byte();
    if ((uint8_t)(len + lcs) != 0) {
        _cs_high();
        return -1;
    }

    // Read data bytes
    int n = (len < buf_size) ? len : buf_size;
    for (int i = 0; i < n; i++)
        buf[i] = _read_byte();
    // Drain extra bytes if buffer was too small
    for (int i = n; i < len; i++)
        _read_byte();

    _read_byte(); // DCS (ignore)
    _read_byte(); // postamble

    _cs_high();
    return n;
}

// ---------------------------------------------------------------------------
// Send command, wait for ACK, wait for response
// cmd[0] = TFI (0xD4), cmd[1] = command, cmd[2..] = params
// ---------------------------------------------------------------------------
static int _command(const uint8_t *cmd, uint8_t cmd_len,
                     uint8_t *resp, int resp_size,
                     int timeout_ms) {
    _write_frame(cmd, cmd_len);

    if (!_read_ack()) {
        return -1;
    }

    if (!_wait_ready(timeout_ms)) {
        return -1;
    }

    return _read_response(resp, resp_size);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void pn532_init(const PN532Config *cfg) {
    _cfg = *cfg;

    // GPIO setup
    gpio_init(_cfg.pin_cs);  gpio_set_dir(_cfg.pin_cs,  GPIO_OUT);
    gpio_put(_cfg.pin_cs, 1);
    gpio_init(_cfg.pin_rst); gpio_set_dir(_cfg.pin_rst, GPIO_OUT);
    gpio_put(_cfg.pin_rst, 1);

    // SPI at 1 MHz — PN532 supports up to 5 MHz
    spi_init(_cfg.spi, 1000 * 1000);
    spi_set_format(_cfg.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(_cfg.pin_sck,  GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_miso, GPIO_FUNC_SPI);

    // Hardware reset
    gpio_put(_cfg.pin_rst, 0); sleep_ms(100);
    gpio_put(_cfg.pin_rst, 1); sleep_ms(100);

    // SPI wakeup: toggle CS
    _cs_low(); sleep_ms(2); _cs_high(); sleep_ms(10);

    uint8_t resp[16];
    int n;

    // --- GetFirmwareVersion: verify communication ---
    uint8_t cmd_fw[] = { TFI_HOST_TO_PN, CMD_GET_FIRMWARE_VERSION };
    n = _command(cmd_fw, 2, resp, sizeof(resp), 200);
    if (n >= 6 && resp[0] == TFI_PN_TO_HOST && resp[1] == 0x03) {
        printf("[PN532] IC=0x%02X FW=%d.%d Features=0x%02X\n",
               resp[2], resp[3], resp[4], resp[5]);
    } else {
        printf("[PN532] ERROR: firmware version failed (n=%d)\n", n);
        if (n > 0) {
            printf("[PN532] Raw:");
            for (int i = 0; i < n; i++) printf(" %02X", resp[i]);
            printf("\n");
        }
    }

    // --- SAMConfiguration: normal mode, no timeout, no IRQ ---
    uint8_t cmd_sam[] = { TFI_HOST_TO_PN, CMD_SAM_CONFIGURATION, 0x01, 0x00, 0x00 };
    n = _command(cmd_sam, 5, resp, sizeof(resp), 200);
    if (n >= 2 && resp[0] == TFI_PN_TO_HOST && resp[1] == 0x15) {
        printf("[PN532] SAM configured (normal mode)\n");
    } else {
        printf("[PN532] SAM config failed\n");
    }

    // --- RFConfiguration: fast passive retry (MaxRtyPassiveActivation = 1) ---
    // This makes InListPassiveTarget return quickly when no card present
    // CfgItem=0x05, MxRtyATR=0xFF, MxRtyPSL=0x01, MxRtyPassiveActivation=0x01
    uint8_t cmd_rf[] = { TFI_HOST_TO_PN, CMD_RF_CONFIGURATION,
                          0x05, 0xFF, 0x01, 0x01 };
    n = _command(cmd_rf, 6, resp, sizeof(resp), 200);
    if (n >= 2 && resp[0] == TFI_PN_TO_HOST && resp[1] == 0x33) {
        printf("[PN532] RF config: fast retry mode\n");
    } else {
        printf("[PN532] RF config failed\n");
    }

    printf("[PN532] Ready\n");
}

int pn532_read_card(uint8_t uid_out[4]) {
    // InListPassiveTarget: 1 target, 106 kbps type A (ISO14443A)
    uint8_t cmd[] = { TFI_HOST_TO_PN, CMD_IN_LIST_PASSIVE, 0x01, 0x00 };
    uint8_t resp[32];

    // Short timeout — with fast retry config, no-card returns in ~20ms
    int n = _command(cmd, 4, resp, sizeof(resp), 150);

    if (n < 2) return PN532_NO_CARD;

    // Response: D5 4B NbTg [Tg SENS_RES SEL_RES NfcIdLen NFCID...]
    if (resp[0] != TFI_PN_TO_HOST || resp[1] != 0x4B)
        return PN532_ERR;

    if (n < 3) return PN532_NO_CARD;

    uint8_t nb_targets = resp[2];
    if (nb_targets == 0)
        return PN532_NO_CARD;

    // Parse UID
    // resp[3] = target number (1)
    // resp[4..5] = SENS_RES / ATQA
    // resp[6] = SEL_RES / SAK
    // resp[7] = UID length
    // resp[8..] = UID bytes
    if (n < 8) return PN532_ERR;

    uint8_t uid_len = resp[7];
    if (uid_len < 4 || n < (int)(8 + uid_len))
        return PN532_ERR;

    memcpy(uid_out, &resp[8], 4);
    return PN532_OK;
}

void pn532_halt(void) {
    // Release the target so it can be detected again
    uint8_t cmd[] = { TFI_HOST_TO_PN, CMD_IN_RELEASE, 0x00 };
    uint8_t resp[8];
    _command(cmd, 3, resp, sizeof(resp), 100);
    sleep_ms(10);
}
