#pragma once

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdint.h>

#define PN532_OK        0
#define PN532_ERR       1
#define PN532_NO_CARD   2

typedef struct {
    spi_inst_t *spi;
    uint pin_sck, pin_mosi, pin_miso, pin_cs, pin_rst;
} PN532Config;

void pn532_init(const PN532Config *cfg);
int  pn532_read_card(uint8_t uid_out[4]);
void pn532_halt(void);
