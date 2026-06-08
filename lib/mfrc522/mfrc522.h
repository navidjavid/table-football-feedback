#pragma once

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdint.h>
#include <stdbool.h>

#define RC522_OK         0
#define RC522_ERR        1
#define RC522_NO_CARD    2

typedef struct {
    spi_inst_t *spi;
    uint pin_sck, pin_mosi, pin_miso, pin_cs, pin_rst;
} RC522Config;

void rc522_init(const RC522Config *cfg);
int  rc522_read_card(uint8_t uid_out[4]);
void rc522_halt(void);
