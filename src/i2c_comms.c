#include "i2c_comms.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/i2c_slave.h"
#include <string.h>
#include <stdio.h>

#define I2C_SDA_PIN  4
#define I2C_SCL_PIN  5
#define I2C_PORT     i2c0
#define RING_SIZE    256

// The ball-tracker camera streams continuously at this bus speed — a
// 20-byte ball packet (I2C_PACKET_SIZE) takes roughly
// 20 bytes * 9 bits/byte / I2C_BAUD seconds, ~19ms here, essentially
// back-to-back with little to no idle gap between frames. So the bus
// isn't "occasionally" busy when a peer-tap send wants it, it is
// continuously busy by design — a short retry (a few hundred us) almost
// always lands mid-frame and just collides again immediately. The RP2040
// I2C hardware itself waits for the bus to go idle (a STOP condition)
// before issuing its own START once put in master mode, so what actually
// matters is giving ONE attempt a long enough timeout to span past a
// full camera frame, not firing off many quick ones.
#define I2C_BAUD              9600
#define PEER_TAP_TIMEOUT_US   40000

static volatile uint8_t _ring[RING_SIZE];
static volatile int     _head = 0;
static volatile int     _tail = 0;

// Small queue for peer-tap messages pulled out of the same byte stream —
// see the header for why these share a bus/address with ball data.
#define PEER_QUEUE_LEN 4
typedef struct { char side; uint8_t slot; uint8_t uid[4]; } PeerTap;
static PeerTap _peer_queue[PEER_QUEUE_LEN];
static int     _peer_head = 0;
static int     _peer_tail = 0;

static void _push_peer_tap(char side, uint8_t slot, const uint8_t uid[4]) {
    int next = (_peer_head + 1) % PEER_QUEUE_LEN;
    if (next == _peer_tail) return; // full — drop; taps are rare, this is fine
    _peer_queue[_peer_head].side = side;
    _peer_queue[_peer_head].slot = slot;
    memcpy(_peer_queue[_peer_head].uid, uid, 4);
    _peer_head = next;
}

bool i2c_comms_get_peer_tap(char *side, uint8_t *slot, uint8_t uid[4]) {
    if (_peer_tail == _peer_head) return false;
    *side = _peer_queue[_peer_tail].side;
    *slot = _peer_queue[_peer_tail].slot;
    memcpy(uid, _peer_queue[_peer_tail].uid, 4);
    _peer_tail = (_peer_tail + 1) % PEER_QUEUE_LEN;
    return true;
}

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
    i2c_init(I2C_PORT, I2C_BAUD);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    i2c_slave_init(I2C_PORT, I2C_SLAVE_ADDR, &_handler);
}

// Both lines idle (high) is the only safe moment to disable our own slave
// interface. GPIO_FUNC_I2C still leaves the raw input path readable via
// gpio_get() regardless of the pin's function-select, so this doesn't
// require touching the I2C peripheral at all to check. Confirmed cause of
// a full game freeze in the field: deiniting our slave interface *mid-byte*
// of an in-flight camera transfer can wedge the shared bus for every
// device on it (camera + both boards), not just drop our own packet — no
// write timeout, however generous, fixes that, because the damage is done
// by the deinit call itself, before the write is ever attempted.
static bool _bus_idle(void) {
    for (int i = 0; i < 20; i++) {
        if (!gpio_get(I2C_SCL_PIN) || !gpio_get(I2C_SDA_PIN)) return false;
        sleep_us(20);
    }
    return true;
}

void i2c_comms_send_peer_tap(char side, uint8_t slot, const uint8_t uid[4]) {
    uint8_t p[REG_PACKET_SIZE];
    p[0] = REG_SYNC_A;
    p[1] = REG_SYNC_B;
    p[2] = (uint8_t)side;
    p[3] = slot;
    p[4] = uid[0]; p[5] = uid[1]; p[6] = uid[2]; p[7] = uid[3];
    p[8] = (uint8_t)(p[2] ^ p[3] ^ p[4] ^ p[5] ^ p[6] ^ p[7]);

    // The camera streams continuously during an active game with near-zero
    // gaps between frames (see memory: i2c-bus-architecture), so this can
    // take a little hunting — that's expected, not a bug. Give up after a
    // bounded budget rather than blocking the game loop indefinitely; the
    // local tap-in/tap-out still applies to this board either way; the
    // sibling just won't hear about it from this attempt.
    uint32_t wait_start = to_ms_since_boot(get_absolute_time());
    while (!_bus_idle()) {
        if (to_ms_since_boot(get_absolute_time()) - wait_start > 100) {
            printf("[I2C] Peer-tap send SKIPPED — bus never went idle side=%c slot=%d\n",
                   side, slot);
            return;
        }
    }

    // Documented idiom (pico/i2c_slave.h): deinit restores master mode,
    // init re-arms slave mode + IRQ. Brief window where we can't receive
    // an incoming ball packet — acceptable, see header note. Safe now
    // because _bus_idle() above already confirmed nothing is mid-transfer.
    i2c_slave_deinit(I2C_PORT);

    // The write's return value used to be discarded entirely, so a failed
    // send (NACK, arbitration lost to the ball-tracker's own write) was
    // silently indistinguishable from "worked fine" — the sibling board's
    // roster would just never update with no way to tell why. One retry
    // for the rare case the camera won the race to START right after we
    // saw idle.
    int rc = PICO_ERROR_GENERIC;
    for (int attempt = 0; attempt < 2 && rc < 0; attempt++) {
        rc = i2c_write_timeout_us(I2C_PORT, I2C_SLAVE_ADDR, p, sizeof(p), false,
                                   PEER_TAP_TIMEOUT_US);
    }
    if (rc < 0) {
        printf("[I2C] Peer-tap send FAILED after retries (rc=%d) side=%c slot=%d\n",
               rc, side, slot);
    } else {
        printf("[I2C] Peer-tap sent OK side=%c slot=%d\n", side, slot);
    }

    i2c_slave_init(I2C_PORT, I2C_SLAVE_ADDR, &_handler);
}

void i2c_comms_poll(BallData *out) {
    out->valid = false;
    int avail = (_head - _tail + RING_SIZE) % RING_SIZE;

    // Scan for either a ball packet (SYNC_A/SYNC_B) or a peer-tap message
    // (REG_SYNC_A/REG_SYNC_B) — they share this one byte stream. A ball
    // packet match returns immediately (existing behavior, one BallData
    // per call); a peer-tap match is queued and scanning continues, since
    // the caller here only wants ball data back.
    while (avail >= 2) {
        uint8_t b0 = _ring[_tail % RING_SIZE];
        uint8_t b1 = _ring[(_tail + 1) % RING_SIZE];

        if (b0 == SYNC_A && b1 == SYNC_B) {
            if (avail < I2C_PACKET_SIZE) break; // wait for the rest to arrive

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

        if (b0 == REG_SYNC_A && b1 == REG_SYNC_B) {
            if (avail < REG_PACKET_SIZE) break; // wait for the rest to arrive

            uint8_t p[REG_PACKET_SIZE];
            for (int i = 0; i < REG_PACKET_SIZE; i++)
                p[i] = _ring[(_tail + i) % RING_SIZE];
            _tail = (_tail + REG_PACKET_SIZE) % RING_SIZE;
            avail -= REG_PACKET_SIZE;

            uint8_t sum = (uint8_t)(p[2] ^ p[3] ^ p[4] ^ p[5] ^ p[6] ^ p[7]);
            if (sum == p[8]) {
                printf("[I2C] Peer-tap header seen, checksum OK, side=%c slot=%d\n",
                       (char)p[2], p[3]);
                _push_peer_tap((char)p[2], p[3], &p[4]);
            } else {
                printf("[I2C] Peer-tap header seen but checksum FAILED (got %02X want %02X)\n",
                       p[8], sum);
            }
            continue;
        }

        // Neither header matched at this position — resync by one byte.
        _tail = (_tail + 1) % RING_SIZE;
        avail--;
    }
}

// Flush ring buffer — call before a new game starts to clear stale bytes
void i2c_comms_flush(void) {
    _tail = _head;
}
