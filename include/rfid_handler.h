#pragma once
#include <stdint.h>
#include <stdbool.h>

#define RFID_UID_LEN  4

typedef enum {
    RFID_NONE = 0,
    RFID_NEW_TAP,
} RFIDResult;

typedef struct {
    RFIDResult result;
    uint8_t    uid[RFID_UID_LEN];
} RFIDScanResult;

void rfid_handler_init(void);
void rfid_handler_scan(RFIDScanResult *out);
