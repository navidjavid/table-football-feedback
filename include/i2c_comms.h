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

// ---------------------------------------------------------------------------
// Peer-to-peer "a card was tapped on the other side" messages, shared on the
// SAME physical I2C bus/address as the ball-tracker feed — there's no spare
// wiring on the assembled PCBs for a dedicated link. Distinguished from ball
// packets purely by a different sync header (REG_SYNC_A/B), same trick the
// ball protocol already uses for framing.
//
// The ball-tracker hardware is the only permanent bus master, always
// WRITING to I2C_SLAVE_ADDR; both side-boards listen there as slaves. To
// send a peer-tap message, a side-board briefly becomes the master itself
// (i2c_slave_deinit() -> i2c_write -> i2c_slave_init() again) and writes to
// that same shared address; the sibling board, still in its normal slave
// listening state, receives it exactly like a ball packet, just with a
// different header. Standard I2C multi-master arbitration handles the rare
// case of colliding with the ball-tracker's own write; a lost/garbled
// attempt is simply dropped — a tap is a one-off, low-frequency event, so
// this is an acceptable, low-stakes failure mode.
#define REG_SYNC_A       0xBB
#define REG_SYNC_B       0x66
#define REG_PACKET_SIZE  9   // sync(2) + side(1) + slot(1) + uid(4) + checksum(1)

void i2c_comms_init(void);
void i2c_comms_poll(BallData *out);
void i2c_comms_flush(void);

// Broadcasts a "this UID just tapped in on my side" message to the sibling
// board. Best-effort — failures are silently dropped (see header note).
void i2c_comms_send_peer_tap(char side, uint8_t slot, const uint8_t uid[4]);

// Drains one pending peer-tap message received from the sibling board, if
// any. Returns false (and leaves outputs untouched) when the queue is
// empty. Call in a loop to drain multiple if several arrived.
bool i2c_comms_get_peer_tap(char *side, uint8_t *slot, uint8_t uid[4]);
