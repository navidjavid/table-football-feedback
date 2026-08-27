#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"

/* ---------------------------------------------------------
 * PN532 <-> Raspberry Pi Pico 2 W
 * --------------------------------------------------------- */
#define PN532_SPI       spi1

#define PN532_SCK       10
#define PN532_MOSI      11
#define PN532_MISO      12
#define PN532_CS        13

/*
 * IMPORTANT:
 * On the Elechouse-style PN532 board, RSTO is RSTOUT_N.
 * It is an OUTPUT from the PN532.
 * Therefore GP15 must NOT drive it.
 */
#define PN532_RSTO      15

#define PN532_SPI_SPEED 1000000

/* PN532 SPI operation bytes */
#define PN532_SPI_DATA_WRITE  0x01
#define PN532_SPI_STATUS_READ 0x02
#define PN532_SPI_DATA_READ   0x03

#define PN532_HOST_TO_PN532   0xD4
#define PN532_PN532_TO_HOST   0xD5

#define PN532_CMD_GET_FIRMWARE_VERSION 0x02
#define PN532_CMD_SAM_CONFIGURATION    0x14
#define PN532_CMD_INLIST_PASSIVE       0x4A


/* ---------------------------------------------------------
 * Bit reversal
 *
 * PN532 SPI is LSB-first.
 * Pico SPI hardware is being used MSB-first, so reverse
 * every byte before TX and after RX.
 * --------------------------------------------------------- */
static uint8_t reverse_byte(uint8_t x)
{
    x = (uint8_t)(((x & 0xF0u) >> 4u) |
                  ((x & 0x0Fu) << 4u));

    x = (uint8_t)(((x & 0xCCu) >> 2u) |
                  ((x & 0x33u) << 2u));

    x = (uint8_t)(((x & 0xAAu) >> 1u) |
                  ((x & 0x55u) << 1u));

    return x;
}


/* ---------------------------------------------------------
 * Transfer one logical PN532 byte
 * --------------------------------------------------------- */
static uint8_t pn532_spi_transfer(uint8_t value)
{
    uint8_t tx = reverse_byte(value);
    uint8_t rx = 0;

    spi_write_read_blocking(PN532_SPI, &tx, &rx, 1);

    return reverse_byte(rx);
}


static inline void pn532_select(void)
{
    gpio_put(PN532_CS, 0);
    /*
     * Elechouse's reference driver re-applies this settle delay before
     * EVERY CS-low transaction (isReady, writeFrame, readAckFrame,
     * readResponse), not just once at boot. Our version was only doing
     * this once during init, so a later poll could hit the chip while
     * it had already dropped back into a low-power idle state, causing
     * the first clocked bytes to be missed and the whole frame rejected.
     */
    sleep_ms(2);
}


static inline void pn532_deselect(void)
{
    gpio_put(PN532_CS, 1);
}


/* ---------------------------------------------------------
 * Check PN532 READY status
 * --------------------------------------------------------- */
static bool pn532_ready(void)
{
    uint8_t status;

    pn532_select();

    pn532_spi_transfer(PN532_SPI_STATUS_READ);
    status = pn532_spi_transfer(0x00);

    pn532_deselect();

    return ((status & 0x01u) != 0u);
}


/* ---------------------------------------------------------
 * Wait until PN532 says a frame is available
 * --------------------------------------------------------- */
static bool pn532_wait_ready(uint32_t timeout_ms)
{
    absolute_time_t timeout =
        make_timeout_time_ms(timeout_ms);

    while (!time_reached(timeout))
    {
        if (pn532_ready())
        {
            return true;
        }

        sleep_ms(2);
    }

    return false;
}


/* ---------------------------------------------------------
 * Write raw PN532 frame
 * --------------------------------------------------------- */
static void pn532_write_raw(
    const uint8_t *buffer,
    size_t length)
{
    pn532_select();

    pn532_spi_transfer(PN532_SPI_DATA_WRITE);

    for (size_t i = 0; i < length; i++)
    {
        pn532_spi_transfer(buffer[i]);
    }

    pn532_deselect();
}


/* ---------------------------------------------------------
 * Read ACK frame
 *
 * Expected:
 * 00 00 FF 00 FF 00
 * --------------------------------------------------------- */
static bool pn532_read_ack(void)
{
    static const uint8_t expected_ack[6] =
    {
        0x00,
        0x00,
        0xFF,
        0x00,
        0xFF,
        0x00
    };

    uint8_t ack[6];

    if (!pn532_wait_ready(1000))
    {
        printf("ERROR: timeout waiting for ACK\n");
        return false;
    }

    pn532_select();

    pn532_spi_transfer(PN532_SPI_DATA_READ);

    for (int i = 0; i < 6; i++)
    {
        ack[i] = pn532_spi_transfer(0x00);
    }

    pn532_deselect();

    printf("ACK: ");

    for (int i = 0; i < 6; i++)
    {
        printf("%02X ", ack[i]);
    }

    printf("\n");

    if (memcmp(ack, expected_ack, 6) != 0)
    {
        printf("ERROR: invalid ACK\n");
        return false;
    }

    return true;
}


/* ---------------------------------------------------------
 * Read a normal PN532 information frame
 *
 * Returns number of DATA bytes.
 * DATA includes:
 *
 * D5 RESPONSE_CODE ...
 * --------------------------------------------------------- */
static int pn532_read_frame(
    uint8_t *data,
    size_t max_data,
    uint32_t timeout_ms)
{
    if (!pn532_wait_ready(timeout_ms))
    {
        return -1;
    }

    pn532_select();

    pn532_spi_transfer(PN532_SPI_DATA_READ);

    uint8_t preamble = pn532_spi_transfer(0x00);
    uint8_t start1   = pn532_spi_transfer(0x00);
    uint8_t start2   = pn532_spi_transfer(0x00);
    uint8_t length   = pn532_spi_transfer(0x00);
    uint8_t lcs      = pn532_spi_transfer(0x00);

    if ((preamble != 0x00) ||
        (start1   != 0x00) ||
        (start2   != 0xFF))
    {
        pn532_deselect();

        printf("ERROR: bad frame header: "
               "%02X %02X %02X\n",
               preamble,
               start1,
               start2);

        return -2;
    }

    if ((uint8_t)(length + lcs) != 0x00)
    {
        pn532_deselect();

        printf("ERROR: LEN checksum\n");

        return -3;
    }

    if (length > max_data)
    {
        pn532_deselect();

        printf("ERROR: response too large\n");

        return -4;
    }

    uint8_t sum = 0;

    for (uint8_t i = 0; i < length; i++)
    {
        data[i] = pn532_spi_transfer(0x00);
        sum = (uint8_t)(sum + data[i]);
    }

    uint8_t dcs =
        pn532_spi_transfer(0x00);

    uint8_t postamble =
        pn532_spi_transfer(0x00);

    pn532_deselect();

    if ((uint8_t)(sum + dcs) != 0x00)
    {
        printf("ERROR: DATA checksum\n");
        return -5;
    }

    if (postamble != 0x00)
    {
        printf("ERROR: bad postamble\n");
        return -6;
    }

    return length;
}


/* ---------------------------------------------------------
 * Send PN532 command
 *
 * command = e.g. 0x02
 *
 * params = bytes after command
 *
 * response[] receives only response payload:
 *
 * D5 CMD+1 is removed.
 * --------------------------------------------------------- */
static int pn532_command(
    uint8_t command,
    const uint8_t *params,
    size_t params_len,
    uint8_t *response,
    size_t response_max,
    uint32_t response_timeout_ms)
{
    uint8_t frame[64];

    /*
     * DATA section:
     *
     * D4 COMMAND PARAMS...
     */
    uint8_t length =
        (uint8_t)(2u + params_len);

    frame[0] = 0x00;             /* preamble */
    frame[1] = 0x00;             /* start code */
    frame[2] = 0xFF;             /* start code */
    frame[3] = length;
    frame[4] = (uint8_t)(0u - length);
    frame[5] = PN532_HOST_TO_PN532;
    frame[6] = command;

    uint8_t checksum =
        (uint8_t)(PN532_HOST_TO_PN532 + command);

    for (size_t i = 0; i < params_len; i++)
    {
        frame[7 + i] = params[i];

        checksum =
            (uint8_t)(checksum + params[i]);
    }

    frame[7 + params_len] =
        (uint8_t)(0u - checksum);

    frame[8 + params_len] =
        0x00;                    /* postamble */

    size_t frame_length =
        9u + params_len;

    pn532_write_raw(frame, frame_length);

    if (!pn532_read_ack())
    {
        return -1;
    }

    uint8_t raw[64];

    int raw_length =
        pn532_read_frame(
            raw,
            sizeof(raw),
            response_timeout_ms);

    if (raw_length < 2)
    {
        printf("ERROR: no/short response\n");
        return -2;
    }

    if (raw[0] != PN532_PN532_TO_HOST)
    {
        printf("ERROR: wrong TFI %02X\n",
               raw[0]);

        return -3;
    }

    if (raw[1] != (uint8_t)(command + 1u))
    {
        printf(
            "ERROR: expected response %02X, got %02X\n",
            (uint8_t)(command + 1u),
            raw[1]);

        return -4;
    }

    int payload_length =
        raw_length - 2;

    if ((size_t)payload_length > response_max)
    {
        payload_length =
            (int)response_max;
    }

    memcpy(
        response,
        &raw[2],
        (size_t)payload_length);

    return payload_length;
}


/* ---------------------------------------------------------
 * Initialise SPI
 * --------------------------------------------------------- */
static void pn532_init_spi(void)
{
    spi_init(
        PN532_SPI,
        PN532_SPI_SPEED);

    spi_set_format(
        PN532_SPI,
        8,
        SPI_CPOL_0,
        SPI_CPHA_0,
        SPI_MSB_FIRST);

    gpio_set_function(
        PN532_SCK,
        GPIO_FUNC_SPI);

    gpio_set_function(
        PN532_MOSI,
        GPIO_FUNC_SPI);

    gpio_set_function(
        PN532_MISO,
        GPIO_FUNC_SPI);


    /* CS controlled manually */
    gpio_init(PN532_CS);

    gpio_put(PN532_CS, 1);

    gpio_set_dir(
        PN532_CS,
        GPIO_OUT);


    /*
     * VERY IMPORTANT:
     *
     * GP15 is connected to RSTO on your PCB.
     * RSTO is an OUTPUT from the PN532 module.
     *
     * Keep Pico GP15 as INPUT.
     */
    gpio_init(PN532_RSTO);

    gpio_set_dir(
        PN532_RSTO,
        GPIO_IN);

    gpio_disable_pulls(
        PN532_RSTO);


    /*
     * Give PN532 time after power-up.
     */
    sleep_ms(100);


    /*
     * Wake via NSS/CS.
     *
     * NXP allows NSS to wake the PN532 when H_REQ
     * is not used.
     */
    pn532_select();
    sleep_ms(5);
    pn532_deselect();

    sleep_ms(10);
}


/* ---------------------------------------------------------
 * Test GetFirmwareVersion
 * --------------------------------------------------------- */
static bool test_firmware(void)
{
    uint8_t response[16];

    printf("\n--- GetFirmwareVersion ---\n");

    int length =
        pn532_command(
            PN532_CMD_GET_FIRMWARE_VERSION,
            NULL,
            0,
            response,
            sizeof(response),
            1000);

    if (length < 4)
    {
        printf("PN532 NOT detected.\n");
        return false;
    }

    printf("IC      : 0x%02X\n", response[0]);
    printf("Firmware: %u.%u\n",
           response[1],
           response[2]);

    printf("Support : 0x%02X\n",
           response[3]);

    if (response[0] == 0x32)
    {
        printf("PN532 detected correctly!\n");
        return true;
    }

    printf("Unexpected IC byte.\n");

    return false;
}


/* ---------------------------------------------------------
 * SAM Normal mode
 * --------------------------------------------------------- */
static bool configure_sam(void)
{
    printf("\n--- SAMConfiguration ---\n");

    /*
     * Mode    = 0x01 -> Normal mode
     * Timeout = 0
     * IRQ     = 0 -> we are polling, IRQ not connected
     */
    uint8_t params[] =
    {
        0x01,
        0x00,
        0x00
    };

    uint8_t response[8];

    int length =
        pn532_command(
            PN532_CMD_SAM_CONFIGURATION,
            params,
            sizeof(params),
            response,
            sizeof(response),
            1000);

    if (length < 0)
    {
        printf("SAMConfiguration FAILED\n");
        return false;
    }

    printf("SAMConfiguration OK\n");

    return true;
}


/* ---------------------------------------------------------
 * Scan one ISO14443-A / MIFARE card
 * --------------------------------------------------------- */
static void scan_card(void)
{
    /*
     * MaxTg = 1
     * BrTy  = 0x00 -> 106 kbit/s Type A
     */
    uint8_t params[] =
    {
        0x01,
        0x00
    };

    uint8_t response[64];

    int length =
        pn532_command(
            PN532_CMD_INLIST_PASSIVE,
            params,
            sizeof(params),
            response,
            sizeof(response),
            1000);

    if (length < 0)
    {
        printf("No card / timeout\n");
        return;
    }

    if (length < 1 ||
        response[0] == 0)
    {
        printf("No card detected\n");
        return;
    }

    /*
     * Response for Type-A:
     *
     * [0] NbTg
     * [1] Tg
     * [2] SENS_RES
     * [3] SENS_RES
     * [4] SEL_RES
     * [5] UID length
     * [6...] UID
     */

    if (length < 6)
    {
        printf("Short card response\n");
        return;
    }

    uint8_t uid_length =
        response[5];

    if ((6 + uid_length) > length)
    {
        printf("Invalid UID length\n");
        return;
    }

    printf("\nCARD DETECTED\n");

    printf("SENS_RES: %02X %02X\n",
           response[2],
           response[3]);

    printf("SEL_RES : %02X\n",
           response[4]);

    printf("UID (%u bytes): ",
           uid_length);

    for (uint8_t i = 0;
         i < uid_length;
         i++)
    {
        printf("%02X",
               response[6 + i]);

        if (i + 1 < uid_length)
        {
            printf(":");
        }
    }

    printf("\n");
}


/* ---------------------------------------------------------
 * MAIN
 * --------------------------------------------------------- */
int main(void)
{
    stdio_init_all();

    /*
     * Give USB serial terminal time to enumerate.
     */
    sleep_ms(2000);

    printf("\n");
    printf("============================\n");
    printf("PN532 Pico 2 W SPI TEST\n");
    printf("============================\n");

    printf("SCK  = GP%d\n", PN532_SCK);
    printf("MOSI = GP%d\n", PN532_MOSI);
    printf("MISO = GP%d\n", PN532_MISO);
    printf("CS   = GP%d\n", PN532_CS);
    printf("SPI  = 1 MHz, Mode 0, LSB-first\n");

    pn532_init_spi();

    if (!test_firmware())
    {
        printf("\nTEST FAILED.\n");

        while (true)
        {
            sleep_ms(1000);
        }
    }

    if (!configure_sam())
    {
        printf("\nSAM TEST FAILED.\n");

        while (true)
        {
            sleep_ms(1000);
        }
    }

    printf("\n");
    printf("Place an NFC/RFID card on reader.\n");

    while (true)
    {
        scan_card();

        sleep_ms(500);
    }
}