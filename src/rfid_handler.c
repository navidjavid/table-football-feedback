#include "rfid_handler.h"
#include "../lib/pn532/pn532.h"
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/regs/intctrl.h"
#include <string.h>

#define RFID_SCK  10
#define RFID_MOSI 11
#define RFID_MISO 12
#define RFID_CS   13
#define RFID_RST  15

#define NO_CARD_THRESHOLD  3

static bool    _tap_active  = false;
static int     _no_card     = 0;
static bool    _fired       = false;
static uint8_t _last_uid[4] = {0};

void rfid_handler_init(void) {
    PN532Config cfg = {
        .spi = spi1, .pin_sck = RFID_SCK, .pin_mosi = RFID_MOSI,
        .pin_miso = RFID_MISO, .pin_cs = RFID_CS, .pin_rst = RFID_RST,
    };
    pn532_init(&cfg);
}

void rfid_handler_scan(RFIDScanResult *out) {
    out->result = RFID_NONE;
    memset(out->uid, 0, 4);

    // Disable I2C IRQ during RFID SPI transaction
    irq_set_enabled(I2C0_IRQ, false);
    uint8_t uid[4];
    int status = pn532_read_card(uid);
    irq_set_enabled(I2C0_IRQ, true);

    if (status == PN532_OK) {
        _no_card = 0;

        if (!_tap_active) {
            _tap_active = true;
            _fired      = false;
        }

        if (!_fired) {
            _fired = true;
            irq_set_enabled(I2C0_IRQ, false);
            pn532_halt();
            irq_set_enabled(I2C0_IRQ, true);

            memcpy(out->uid, uid, 4);
            memcpy(_last_uid, uid, 4);
            out->result = RFID_NEW_TAP;
        }
    } else {
        if (_tap_active) {
            _no_card++;
            if (_no_card >= NO_CARD_THRESHOLD) {
                _tap_active = false;
                _no_card    = 0;
                _fired      = false;
            }
        }
    }
}
