#pragma once

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// RC522 register map
// ---------------------------------------------------------------------------
#define RC522_REG_COMMAND       0x01
#define RC522_REG_COM_IEN       0x02
#define RC522_REG_COM_IRQ       0x04
#define RC522_REG_ERROR         0x06
#define RC522_REG_FIFO_DATA     0x09
#define RC522_REG_FIFO_LEVEL    0x0A
#define RC522_REG_BIT_FRAMING   0x0D
#define RC522_REG_COLL          0x0E
#define RC522_REG_MODE          0x11
#define RC522_REG_TX_CONTROL    0x14
#define RC522_REG_TX_ASK        0x15
#define RC522_REG_T_MODE        0x2A
#define RC522_REG_T_PRESCALER   0x2B
#define RC522_REG_T_RELOAD_H    0x2C
#define RC522_REG_T_RELOAD_L    0x2D
#define RC522_REG_VERSION       0x37

// ---------------------------------------------------------------------------
// Return codes
// ---------------------------------------------------------------------------
#define RC522_OK            0
#define RC522_ERR           1
#define RC522_NO_CARD       2

// ---------------------------------------------------------------------------
// Driver config — pass once to rc522_init()
// ---------------------------------------------------------------------------
typedef struct {
    spi_inst_t *spi;
    uint        pin_sck;
    uint        pin_mosi;
    uint        pin_miso;
    uint        pin_cs;
    uint        pin_rst;
} RC522Config;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Initialise SPI bus and RC522 hardware.
 * Call once at startup before any other rc522_* function.
 */
void rc522_init(const RC522Config *cfg);

/**
 * Scan for a card.
 * @param uid_out  4-byte buffer filled with UID on success.
 * @return RC522_OK if a card was read, RC522_NO_CARD if none present,
 *         RC522_ERR on communication error.
 *
 * The function returns RC522_NO_CARD every call when no card is present,
 * so the caller is responsible for debounce / tap detection.
 */
int rc522_read_card(uint8_t uid_out[4]);

/**
 * Send HLTA and power-cycle the RF field so the same card can be
 * detected again on the next scan cycle.
 * Call this after successfully processing a card read.
 */
void rc522_halt(void);
