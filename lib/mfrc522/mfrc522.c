#include "mfrc522.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static RC522Config _cfg;

// ---------------------------------------------------------------------------
// Low-level SPI helpers
// NOTE: sleep_us after every CS transaction is required for this RC522 clone.
//       The clone needs ~50 µs settling time that MicroPython gets for free
//       from interpreter overhead. Without these delays the IRQ flags and
//       FIFO behave incorrectly.
// ---------------------------------------------------------------------------
static uint8_t _read(uint8_t reg) {
    uint8_t tx = ((reg << 1) & 0x7E) | 0x80;
    uint8_t rx = 0;
    gpio_put(_cfg.pin_cs, 0);
    sleep_us(5);
    spi_write_blocking(_cfg.spi, &tx, 1);
    spi_read_blocking(_cfg.spi, 0x00, &rx, 1);
    gpio_put(_cfg.pin_cs, 1);
    sleep_us(50);
    return rx;
}

static void _write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { (reg << 1) & 0x7E, val };
    gpio_put(_cfg.pin_cs, 0);
    sleep_us(5);
    spi_write_blocking(_cfg.spi, buf, 2);
    gpio_put(_cfg.pin_cs, 1);
    sleep_us(50);
}

static void _set_bit(uint8_t reg, uint8_t mask) {
    _write(reg, _read(reg) | mask);
}

static void _clear_bit(uint8_t reg, uint8_t mask) {
    _write(reg, _read(reg) & (~mask));
}

// ---------------------------------------------------------------------------
// Core transceive
// ---------------------------------------------------------------------------
static uint8_t _transceive(uint8_t *send, uint8_t send_len,
                            uint8_t *recv, uint8_t *recv_len) {
    *recv_len = 0;

    _write(RC522_REG_COM_IEN,    0xFF);
    _write(RC522_REG_COM_IRQ,    0x00);
    _set_bit(RC522_REG_FIFO_LEVEL, 0x80);   // flush FIFO
    _write(RC522_REG_COMMAND,    0x00);      // Idle

    for (uint8_t i = 0; i < send_len; i++)
        _write(RC522_REG_FIFO_DATA, send[i]);

    _write(RC522_REG_COMMAND, 0x0C);         // Transceive
    _set_bit(RC522_REG_BIT_FRAMING, 0x80);  // StartSend

    // Poll for RxIRQ (0x20) or TimerIRQ (0x01).
    // sleep_us(100) per iteration matches MicroPython interpreter speed,
    // which this clone relies on for correct timing.
    uint8_t irq = 0;
    for (int i = 0; i < 300; i++) {
        irq = _read(RC522_REG_COM_IRQ);
        if (irq & 0x01) break;   // timer fired — no card
        if (irq & 0x20) break;   // RxIRQ — data ready
        sleep_us(100);
    }

    _clear_bit(RC522_REG_BIT_FRAMING, 0x80);
    sleep_us(500);

    uint8_t fifo = _read(RC522_REG_FIFO_LEVEL);
    *recv_len = (fifo > 16) ? 16 : fifo;
    for (uint8_t i = 0; i < *recv_len; i++)
        recv[i] = _read(RC522_REG_FIFO_DATA);

    return irq;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void rc522_init(const RC522Config *cfg) {
    _cfg = *cfg;

    // GPIO for CS and RST
    gpio_init(_cfg.pin_cs);
    gpio_set_dir(_cfg.pin_cs, GPIO_OUT);
    gpio_put(_cfg.pin_cs, 1);

    gpio_init(_cfg.pin_rst);
    gpio_set_dir(_cfg.pin_rst, GPIO_OUT);
    gpio_put(_cfg.pin_rst, 1);

    // SPI bus — 100 kHz required for this clone
    spi_init(_cfg.spi, 100 * 1000);
    spi_set_format(_cfg.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(_cfg.pin_sck,  GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_miso, GPIO_FUNC_SPI);

    // Hard reset
    gpio_put(_cfg.pin_rst, 0); sleep_ms(50);
    gpio_put(_cfg.pin_rst, 1); sleep_ms(50);

    _write(RC522_REG_COMMAND,     0x0F);   // SoftReset
    sleep_ms(150);

    // Timer: ~25 ms auto-timeout
    _write(RC522_REG_T_MODE,      0x80);
    _write(RC522_REG_T_PRESCALER, 0xA9);
    _write(RC522_REG_T_RELOAD_H,  0x03);
    _write(RC522_REG_T_RELOAD_L,  0xE8);

    _write(RC522_REG_TX_ASK,      0x40);   // 100% ASK
    _write(RC522_REG_MODE,        0x3D);   // CRC preset 6363

    // Antenna on
    if (!(_read(RC522_REG_TX_CONTROL) & 0x03))
        _set_bit(RC522_REG_TX_CONTROL, 0x03);

    sleep_ms(50);
}

int rc522_read_card(uint8_t uid_out[4]) {
    // --- Step 1: REQA ---
    _write(RC522_REG_BIT_FRAMING, 0x07);   // 7-bit frame
    uint8_t send[1] = { 0x26 };
    uint8_t recv[16];
    uint8_t recv_len = 0;

    uint8_t irq = _transceive(send, 1, recv, &recv_len);

    if (!(irq & 0x20) && recv_len < 2)
        return RC522_NO_CARD;

    // --- Step 2: Anticollision ---
    sleep_ms(2);
    _write(RC522_REG_BIT_FRAMING, 0x00);
    _clear_bit(RC522_REG_COLL, 0x80);

    uint8_t ac_send[2] = { 0x93, 0x20 };
    uint8_t uid_buf[16];
    uint8_t uid_len = 0;

    _transceive(ac_send, 2, uid_buf, &uid_len);

    if (uid_len < 5)
        return RC522_ERR;

    // Verify checksum byte
    uint8_t chk = uid_buf[0] ^ uid_buf[1] ^ uid_buf[2] ^ uid_buf[3];
    if (chk != uid_buf[4])
        return RC522_ERR;

    memcpy(uid_out, uid_buf, 4);
    return RC522_OK;
}

void rc522_halt(void) {
    sleep_ms(2);
    _write(RC522_REG_BIT_FRAMING, 0x00);
    uint8_t send[2] = { 0x50, 0x00 };
    uint8_t recv[16];
    uint8_t recv_len = 0;
    _transceive(send, 2, recv, &recv_len);

    // Power-cycle the RF field — forces card to reset so it
    // can be detected again on the next tap
    _clear_bit(RC522_REG_TX_CONTROL, 0x03);
    sleep_ms(10);
    _set_bit(RC522_REG_TX_CONTROL, 0x03);
    sleep_ms(10);
}
