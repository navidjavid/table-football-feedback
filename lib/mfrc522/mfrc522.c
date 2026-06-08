#include "mfrc522.h"
#include <string.h>

// RC522 registers
#define REG_COMMAND       0x01
#define REG_COM_IEN       0x02
#define REG_COM_IRQ       0x04
#define REG_ERROR         0x06
#define REG_FIFO_DATA     0x09
#define REG_FIFO_LEVEL    0x0A
#define REG_BIT_FRAMING   0x0D
#define REG_COLL          0x0E
#define REG_MODE          0x11
#define REG_TX_CONTROL    0x14
#define REG_TX_ASK        0x15
#define REG_T_MODE        0x2A
#define REG_T_PRESCALER   0x2B
#define REG_T_RELOAD_H    0x2C
#define REG_T_RELOAD_L    0x2D

static RC522Config _cfg;

static uint8_t _read(uint8_t reg) {
    uint8_t tx = ((reg << 1) & 0x7E) | 0x80;
    uint8_t rx = 0;
    gpio_put(_cfg.pin_cs, 0); sleep_us(5);
    spi_write_blocking(_cfg.spi, &tx, 1);
    spi_read_blocking(_cfg.spi, 0x00, &rx, 1);
    gpio_put(_cfg.pin_cs, 1); sleep_us(50);
    return rx;
}

static void _write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { (reg << 1) & 0x7E, val };
    gpio_put(_cfg.pin_cs, 0); sleep_us(5);
    spi_write_blocking(_cfg.spi, buf, 2);
    gpio_put(_cfg.pin_cs, 1); sleep_us(50);
}

static void _set_bit(uint8_t reg, uint8_t mask)   { _write(reg, _read(reg) | mask); }
static void _clear_bit(uint8_t reg, uint8_t mask) { _write(reg, _read(reg) & ~mask); }

static uint8_t _transceive(uint8_t *send, uint8_t send_len,
                            uint8_t *recv, uint8_t *recv_len) {
    *recv_len = 0;
    _write(REG_COM_IEN, 0xFF);
    _write(REG_COM_IRQ, 0x00);
    _set_bit(REG_FIFO_LEVEL, 0x80);
    _write(REG_COMMAND, 0x00);

    for (uint8_t i = 0; i < send_len; i++)
        _write(REG_FIFO_DATA, send[i]);

    _write(REG_COMMAND, 0x0C);
    _set_bit(REG_BIT_FRAMING, 0x80);

    uint8_t irq = 0;
    for (int i = 0; i < 300; i++) {
        irq = _read(REG_COM_IRQ);
        if (irq & 0x01) break;
        if (irq & 0x20) break;
        sleep_us(100);
    }

    _clear_bit(REG_BIT_FRAMING, 0x80);
    sleep_us(500);

    uint8_t fifo = _read(REG_FIFO_LEVEL);
    *recv_len = (fifo > 16) ? 16 : fifo;
    for (uint8_t i = 0; i < *recv_len; i++)
        recv[i] = _read(REG_FIFO_DATA);

    if (irq & 0x20) return RC522_OK;
    if (irq & 0x01) return RC522_NO_CARD;
    return RC522_ERR;
}

void rc522_init(const RC522Config *cfg) {
    _cfg = *cfg;

    gpio_init(_cfg.pin_cs);  gpio_set_dir(_cfg.pin_cs,  GPIO_OUT); gpio_put(_cfg.pin_cs, 1);
    gpio_init(_cfg.pin_rst); gpio_set_dir(_cfg.pin_rst, GPIO_OUT); gpio_put(_cfg.pin_rst, 1);

    spi_init(_cfg.spi, 100 * 1000);
    spi_set_format(_cfg.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(_cfg.pin_sck,  GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_mosi, GPIO_FUNC_SPI);
    gpio_set_function(_cfg.pin_miso, GPIO_FUNC_SPI);

    gpio_put(_cfg.pin_rst, 0); sleep_ms(50);
    gpio_put(_cfg.pin_rst, 1); sleep_ms(50);

    _write(REG_COMMAND, 0x0F); sleep_ms(150);
    _write(REG_T_MODE,      0x80);
    _write(REG_T_PRESCALER, 0xA9);
    _write(REG_T_RELOAD_H,  0x03);
    _write(REG_T_RELOAD_L,  0xE8);
    _write(REG_TX_ASK,      0x40);
    _write(REG_MODE,        0x3D);
    if (!(_read(REG_TX_CONTROL) & 0x03))
        _set_bit(REG_TX_CONTROL, 0x03);
    sleep_ms(50);
}

int rc522_read_card(uint8_t uid_out[4]) {
    _write(REG_BIT_FRAMING, 0x07);
    uint8_t send[1] = { 0x26 };
    uint8_t recv[16];
    uint8_t recv_len = 0;
    uint8_t irq = _transceive(send, 1, recv, &recv_len);

    if (!(irq == RC522_OK) && recv_len < 2)
        return RC522_NO_CARD;

    sleep_ms(2);
    _write(REG_BIT_FRAMING, 0x00);
    _clear_bit(REG_COLL, 0x80);

    uint8_t ac_send[2] = { 0x93, 0x20 };
    uint8_t uid_buf[16];
    uint8_t uid_len = 0;
    _transceive(ac_send, 2, uid_buf, &uid_len);

    if (uid_len < 5) return RC522_ERR;

    uint8_t chk = uid_buf[0] ^ uid_buf[1] ^ uid_buf[2] ^ uid_buf[3];
    if (chk != uid_buf[4]) return RC522_ERR;

    memcpy(uid_out, uid_buf, 4);
    return RC522_OK;
}

void rc522_halt(void) {
    sleep_ms(2);
    _write(REG_BIT_FRAMING, 0x00);
    uint8_t send[2] = { 0x50, 0x00 };
    uint8_t recv[16];
    uint8_t recv_len = 0;
    _transceive(send, 2, recv, &recv_len);
    _clear_bit(REG_TX_CONTROL, 0x03);
    sleep_ms(10);
    _set_bit(REG_TX_CONTROL, 0x03);
    sleep_ms(10);
}
