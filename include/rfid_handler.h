#pragma once

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Player identification via RFID
// ---------------------------------------------------------------------------

// Maximum number of registered players
#define RFID_MAX_PLAYERS  8

// UID is 4 bytes
#define RFID_UID_LEN      4

typedef struct {
    uint8_t uid[RFID_UID_LEN];
    char    name[32];
} RFIDPlayer;

// Result returned by rfid_handler_scan()
typedef enum {
    RFID_RESULT_NONE = 0,   // no card present
    RFID_RESULT_NEW_TAP,    // same card tapped again (re-tap after removal)
    RFID_RESULT_KNOWN,      // registered player identified
    RFID_RESULT_UNKNOWN,    // unregistered card
} RFIDResult;

typedef struct {
    RFIDResult  result;
    uint8_t     uid[RFID_UID_LEN];
    const char *player_name;   // NULL if unknown / none
    int         player_index;  // -1 if unknown / none
} RFIDScanResult;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

/**
 * Initialise the RFID hardware. Call once at startup.
 */
void rfid_handler_init(void);

/**
 * Register a player UID. Call during setup or when a new player registers.
 * Returns false if the table is full (RFID_MAX_PLAYERS reached).
 */
bool rfid_handler_register_player(const uint8_t uid[RFID_UID_LEN],
                                  const char *name);

/**
 * Scan for a card. Call repeatedly in the main loop.
 *
 * Handles debounce internally:
 *   - While card stays on reader  → returns RFID_RESULT_NONE after first read
 *   - After card is removed       → next tap returns RFID_RESULT_KNOWN/_UNKNOWN
 *   - Re-tap of same card counts  → RFID_RESULT_NEW_TAP
 *
 * @param out  Filled on RFID_RESULT_KNOWN / RFID_RESULT_UNKNOWN / NEW_TAP.
 */
void rfid_handler_scan(RFIDScanResult *out);
