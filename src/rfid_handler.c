#include "rfid_handler.h"
#include "../lib/mfrc522/mfrc522.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
#define RFID_SCK  10
#define RFID_MOSI 11
#define RFID_MISO 12
#define RFID_CS   13
#define RFID_RST  15

// ---------------------------------------------------------------------------
// Tap detection state machine
//
//  IDLE ──(card detected)──► READING ──(uid ok)──► ACTIVE
//                                                      │
//                              ◄──(no card N times)────┘
//
//  ACTIVE : uid is valid, card is on the reader. We fire the event ONCE.
//  After the card leaves (N consecutive no-card reads) we go back to IDLE,
//  so the next tap — even the same card — fires a new event.
// ---------------------------------------------------------------------------
#define NO_CARD_THRESHOLD  3   // consecutive no-card reads before "card gone"

typedef enum {
    TAP_IDLE,
    TAP_ACTIVE,
} TapState;

static TapState _tap_state       = TAP_IDLE;
static uint8_t  _active_uid[4]   = {0};
static int      _no_card_count   = 0;
static bool     _event_fired     = false;

// ---------------------------------------------------------------------------
// Player registry
// ---------------------------------------------------------------------------
static RFIDPlayer _players[RFID_MAX_PLAYERS];
static int        _player_count = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool _uid_eq(const uint8_t a[4], const uint8_t b[4]) {
    return memcmp(a, b, 4) == 0;
}

static int _find_player(const uint8_t uid[4]) {
    for (int i = 0; i < _player_count; i++) {
        if (_uid_eq(_players[i].uid, uid))
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void rfid_handler_init(void) {
    RC522Config cfg = {
        .spi      = spi1,
        .pin_sck  = RFID_SCK,
        .pin_mosi = RFID_MOSI,
        .pin_miso = RFID_MISO,
        .pin_cs   = RFID_CS,
        .pin_rst  = RFID_RST,
    };
    rc522_init(&cfg);
}

bool rfid_handler_register_player(const uint8_t uid[RFID_UID_LEN],
                                   const char *name) {
    if (_player_count >= RFID_MAX_PLAYERS) return false;
    memcpy(_players[_player_count].uid, uid, RFID_UID_LEN);
    strncpy(_players[_player_count].name, name,
            sizeof(_players[0].name) - 1);
    _player_count++;
    return true;
}

void rfid_handler_scan(RFIDScanResult *out) {
    out->result       = RFID_RESULT_NONE;
    out->player_name  = NULL;
    out->player_index = -1;
    memset(out->uid, 0, 4);

    uint8_t uid[4];
    int status = rc522_read_card(uid);

    if (status == RC522_OK) {
        _no_card_count = 0;

        if (_tap_state == TAP_IDLE) {
            // Fresh tap — fire event
            _tap_state    = TAP_ACTIVE;
            _event_fired  = false;
            memcpy(_active_uid, uid, 4);
        }

        if (!_event_fired) {
            // Fire exactly once per tap
            _event_fired = true;
            rc522_halt();

            memcpy(out->uid, uid, 4);
            int idx = _find_player(uid);

            if (idx >= 0) {
                out->result       = RFID_RESULT_KNOWN;
                out->player_name  = _players[idx].name;
                out->player_index = idx;
            } else {
                out->result = RFID_RESULT_UNKNOWN;
            }
        }
        // else: card still on reader, event already fired — return NONE

    } else {
        // No card or error
        if (_tap_state == TAP_ACTIVE) {
            _no_card_count++;
            if (_no_card_count >= NO_CARD_THRESHOLD) {
                // Card has been removed — reset so next tap fires again
                _tap_state     = TAP_IDLE;
                _no_card_count = 0;
                _event_fired   = false;
                memset(_active_uid, 0, 4);
            }
        }
    }
}
