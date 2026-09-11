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
    i2c_init(I2C_PORT, 9600);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    i2c_slave_init(I2C_PORT, I2C_SLAVE_ADDR, &_handler);
}

void i2c_comms_send_peer_tap(char side, uint8_t slot, const uint8_t uid[4]) {
    uint8_t p[REG_PACKET_SIZE];
    p[0] = REG_SYNC_A;
    p[1] = REG_SYNC_B;
    p[2] = (uint8_t)side;
    p[3] = slot;
    p[4] = uid[0]; p[5] = uid[1]; p[6] = uid[2]; p[7] = uid[3];
    p[8] = (uint8_t)(p[2] ^ p[3] ^ p[4] ^ p[5] ^ p[6] ^ p[7]);

    // Documented idiom (pico/i2c_slave.h): deinit restores master mode,
    // init re-arms slave mode + IRQ. Brief window where we can't receive
    // an incoming ball packet — acceptable, see header note.
    i2c_slave_deinit(I2C_PORT);

    // The write's return value used to be discarded entirely, so a failed
    // send (NACK, arbitration lost to the ball-tracker's own write, bus
    // busy) was silently indistinguishable from "worked fine" — the
    // sibling board's roster would just never fill in with no way to tell
    // why. A tap is a one-off event, so a couple of quick retries costs
    // nothing and covers the transient case; a real logged failure still
    // means it's genuinely not getting through.
    int rc = PICO_ERROR_GENERIC;
    for (int attempt = 0; attempt < 3 && rc < 0; attempt++) {
        if (attempt > 0) sleep_us(500);
        rc = i2c_write_timeout_us(I2C_PORT, I2C_SLAVE_ADDR, p, sizeof(p), false, 5000);
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
