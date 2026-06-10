#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SYNC_A           0xAA
#define SYNC_B           0x55
#define I2C_SLAVE_ADDR   0x42
#define I2C_PACKET_SIZE  20

typedef struct {
    uint16_t x, y, prev_x, prev_y;
    uint16_t field_w, field_h;
    float    speed;
    uint8_t  possession;   // 0=none, 1=A, 2=B
    uint8_t  score_a, score_b;
    bool     valid;
} BallData;

void i2c_comms_init(void);
void i2c_comms_poll(BallData *out);
void i2c_comms_flush(void);
