#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

// --- Pins ---
#define RFID_SCK  10
#define RFID_MOSI 11
#define RFID_MISO 12
#define RFID_CS   13
#define RFID_RST  15

// --- Registers ---
#define CommandReg    0x01
#define ComIEnReg     0x02
#define ComIrqReg     0x04
#define ErrorReg      0x06
#define FIFODataReg   0x09
#define FIFOLevelReg  0x0A
#define BitFramingReg 0x0D
#define CollReg       0x0E
#define ModeReg       0x11
#define TxControlReg  0x14
#define TxASKReg      0x15
#define TModeReg      0x2A
#define TPrescalerReg 0x2B
#define TReloadRegH   0x2C
#define TReloadRegL   0x2D
#define VersionReg    0x37

spi_inst_t *_spi;

// --- SPI helpers ---
// sleep_us after every access is critical for this RC522 clone
uint8_t read_reg(uint8_t reg) {
    uint8_t tx = ((reg << 1) & 0x7E) | 0x80;
    uint8_t rx = 0;
    gpio_put(RFID_CS, 0);
    sleep_us(5);
    spi_write_blocking(_spi, &tx, 1);
    spi_read_blocking(_spi, 0x00, &rx, 1);
    gpio_put(RFID_CS, 1);
    sleep_us(50);
    return rx;
}

void write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { (reg << 1) & 0x7E, val };
    gpio_put(RFID_CS, 0);
    sleep_us(5);
    spi_write_blocking(_spi, buf, 2);
    gpio_put(RFID_CS, 1);
    sleep_us(50);
}

void set_bit(uint8_t reg, uint8_t mask) {
    write_reg(reg, read_reg(reg) | mask);
}

void clear_bit(uint8_t reg, uint8_t mask) {
    write_reg(reg, read_reg(reg) & (~mask));
}

// --- Init ---
void rfid_init() {
    gpio_put(RFID_RST, 0); sleep_ms(50);
    gpio_put(RFID_RST, 1); sleep_ms(50);
    write_reg(CommandReg,    0x0F);   // SoftReset
    sleep_ms(150);
    write_reg(TModeReg,      0x80);
    write_reg(TPrescalerReg, 0xA9);
    write_reg(TReloadRegH,   0x03);
    write_reg(TReloadRegL,   0xE8);
    write_reg(TxASKReg,      0x40);
    write_reg(ModeReg,       0x3D);
    if (!(read_reg(TxControlReg) & 0x03))
        set_bit(TxControlReg, 0x03);
    sleep_ms(50);
}

// --- Transceive ---
uint8_t transceive(uint8_t *send, uint8_t send_len,
                   uint8_t *recv, uint8_t *recv_len) {
    *recv_len = 0;
    write_reg(ComIEnReg,  0xFF);
    write_reg(ComIrqReg,  0x00);
    set_bit(FIFOLevelReg, 0x80);
    write_reg(CommandReg, 0x00);

    for (uint8_t i = 0; i < send_len; i++)
        write_reg(FIFODataReg, send[i]);

    write_reg(CommandReg, 0x0C);
    set_bit(BitFramingReg, 0x80);

    uint8_t n = 0;
    for (int i = 0; i < 300; i++) {
        n = read_reg(ComIrqReg);
        if (n & 0x01) break;   // timer = no card
        if (n & 0x20) break;   // RxIRQ = got data
        sleep_us(100);
    }

    clear_bit(BitFramingReg, 0x80);
    sleep_us(500);

    uint8_t fifo = read_reg(FIFOLevelReg);
    *recv_len = (fifo > 16) ? 16 : fifo;
    for (uint8_t i = 0; i < *recv_len; i++)
        recv[i] = read_reg(FIFODataReg);

    return n;
}

// --- Request: check if card present ---
int request_card(uint8_t *atqa) {
    write_reg(BitFramingReg, 0x07);   // 7-bit frame for REQA
    uint8_t send[1] = { 0x26 };
    uint8_t recv[16];
    uint8_t recv_len = 0;

    uint8_t irq = transceive(send, 1, recv, &recv_len);

    if ((irq & 0x20) || recv_len >= 2) {
        atqa[0] = recv_len > 0 ? recv[0] : 0;
        atqa[1] = recv_len > 1 ? recv[1] : 0;
        return 1;
    }
    return 0;
}

// --- Anticollision: read UID ---
int anticoll(uint8_t *uid) {
    sleep_ms(2);
    write_reg(BitFramingReg, 0x00);
    clear_bit(CollReg, 0x80);

    uint8_t send[2] = { 0x93, 0x20 };
    uint8_t recv[16];
    uint8_t recv_len = 0;

    transceive(send, 2, recv, &recv_len);

    if (recv_len >= 5) {
        uint8_t chk = recv[0] ^ recv[1] ^ recv[2] ^ recv[3];
        if (chk == recv[4]) {
            memcpy(uid, recv, 4);
            return 1;
        }
    }
    return 0;
}

// --- Halt: silence card until removed ---
void halt_card() {
    sleep_ms(2);
    write_reg(BitFramingReg, 0x00);
    uint8_t send[2] = { 0x50, 0x00 };
    uint8_t recv[16];
    uint8_t recv_len = 0;
    transceive(send, 2, recv, &recv_len);
    // After halt, turn antenna off and on to fully reset RF field
    clear_bit(TxControlReg, 0x03);
    sleep_ms(5);
    set_bit(TxControlReg, 0x03);
    sleep_ms(5);
}

int main() {
    stdio_init_all();
    sleep_ms(3000);

    printf("\n=== MFRC522 RFID Reader ===\n");

    _spi = spi1;
    spi_init(_spi, 100 * 1000);
    spi_set_format(_spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(RFID_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(RFID_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(RFID_MISO, GPIO_FUNC_SPI);
    gpio_init(RFID_CS);  gpio_set_dir(RFID_CS,  GPIO_OUT); gpio_put(RFID_CS,  1);
    gpio_init(RFID_RST); gpio_set_dir(RFID_RST, GPIO_OUT); gpio_put(RFID_RST, 1);

    rfid_init();

    printf("Version : 0x%02X\n", read_reg(VersionReg));
    printf("Ready - scan a card...\n");
    printf("---------------------------\n");

    char last_uid[12] = {0};

    while (true) {
        uint8_t atqa[2] = {0};

        if (request_card(atqa)) {
            uint8_t uid[4] = {0};
            if (anticoll(uid)) {
                char uid_str[12];
                snprintf(uid_str, sizeof(uid_str), "%02X%02X%02X%02X",
                         uid[0], uid[1], uid[2], uid[3]);

                // Only print once per tap
                if (strcmp(uid_str, last_uid) != 0) {
                    printf("Card UID: %02X:%02X:%02X:%02X\n",
                           uid[0], uid[1], uid[2], uid[3]);
                    strcpy(last_uid, uid_str);
                }
                halt_card();
            }
        } else {
            // No card in field — reset so next tap prints again
            last_uid[0] = 0;
        }

        sleep_ms(200);
    }
}