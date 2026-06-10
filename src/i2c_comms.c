#include "i2c_comms.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/i2c_slave.h"
#include <string.h>

#define I2C_SDA_PIN  4
#define I2C_SCL_PIN  5
#define I2C_PORT     i2c0
#define RING_SIZE    256

static volatile uint8_t _ring[RING_SIZE];
static volatile int     _head = 0;
static volatile int     _tail = 0;

static void _handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    if (event == I2C_SLAVE_RECEIVE) {
        uint8_t b = i2c_read_byte_raw(i2c);
        int next = (_head + 1) % RING_SIZE;
        if (next != _tail) {
            _ring[_head] = b;
            _head = next;
        }
    } else if (event == I2C_SLAVE_REQUEST) {
        i2c_write_byte_raw(i2c, 0x00);
    }
}

void i2c_comms_init(void) {
    i2c_init(I2C_PORT, 9600);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    i2c_slave_init(I2C_PORT, I2C_SLAVE_ADDR, &_handler);
}

void i2c_comms_poll(BallData *out) {
    out->valid = false;
    int avail = (_head - _tail + RING_SIZE) % RING_SIZE;

    while (avail >= I2C_PACKET_SIZE) {
        uint8_t b0 = _ring[_tail % RING_SIZE];
        uint8_t b1 = _ring[(_tail + 1) % RING_SIZE];

        if (b0 != SYNC_A || b1 != SYNC_B) {
            _tail = (_tail + 1) % RING_SIZE;
            avail--;
            continue;
        }

        uint8_t p[I2C_PACKET_SIZE];
        for (int i = 0; i < I2C_PACKET_SIZE; i++)
            p[i] = _ring[(_tail + i) % RING_SIZE];
        _tail = (_tail + I2C_PACKET_SIZE) % RING_SIZE;

        out->x       = ((uint16_t)p[2]  << 8) | p[3];
        out->y       = ((uint16_t)p[4]  << 8) | p[5];
        out->prev_x  = ((uint16_t)p[6]  << 8) | p[7];
        out->prev_y  = ((uint16_t)p[8]  << 8) | p[9];
        out->field_w = ((uint16_t)p[10] << 8) | p[11];
        out->field_h = ((uint16_t)p[12] << 8) | p[13];

        union { uint8_t b[4]; float f; } spd;
        spd.b[0] = p[14]; spd.b[1] = p[15];
        spd.b[2] = p[16]; spd.b[3] = p[17];
        out->speed = spd.f;

        out->possession = p[18];
        out->score_a    = (p[19] >> 4) & 0x0F;
        out->score_b    =  p[19]       & 0x0F;
        out->valid      = true;
        return;
    }
}

// Flush ring buffer — call before a new game starts to clear stale bytes
void i2c_comms_flush(void) {
    _tail = _head;
}
