/**
 * test_i2c_simulator.c — Second Pico, I2C master
 *
 * Simulates ball-tracker board with realistic game data:
 *   - Ball moves around field with varying speed (5-25 km/h)
 *   - Occasional shots (sudden speed spike + direction change)
 *   - Goals when ball reaches a wall
 *   - Realistic pauses after goals (ball resets to centre)
 *
 * Wiring to main Pico:
 *   GP4 (SDA) ── GP4
 *   GP5 (SCL) ── GP5
 *   GND       ── GND
 *   Main VBUS ── Simulator VSYS
 *   4.7kΩ pull-ups SDA & SCL to 3.3V
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define I2C_SDA      4
#define I2C_SCL      5
#define I2C_PORT     i2c0
#define SLAVE_ADDR   0x42
#define PACKET_SIZE  20
#define SYNC_A       0xAA
#define SYNC_B       0x55

#define FIELD_W      1000
#define FIELD_H      500

static void pack_float(uint8_t *buf, float f) {
    union { float f; uint8_t b[4]; } u;
    u.f = f;
    memcpy(buf, u.b, 4);
}

static void build_packet(uint8_t *pkt,
                          uint16_t x,  uint16_t y,
                          uint16_t px, uint16_t py,
                          float speed,
                          uint8_t poss,
                          uint8_t sa, uint8_t sb) {
    pkt[0]  = SYNC_A; pkt[1] = SYNC_B;
    pkt[2]  = (x  >> 8) & 0xFF; pkt[3]  =  x  & 0xFF;
    pkt[4]  = (y  >> 8) & 0xFF; pkt[5]  =  y  & 0xFF;
    pkt[6]  = (px >> 8) & 0xFF; pkt[7]  =  px & 0xFF;
    pkt[8]  = (py >> 8) & 0xFF; pkt[9]  =  py & 0xFF;
    pkt[10] = (FIELD_W >> 8) & 0xFF; pkt[11] = FIELD_W & 0xFF;
    pkt[12] = (FIELD_H >> 8) & 0xFF; pkt[13] = FIELD_H & 0xFF;
    pack_float(&pkt[14], speed);
    pkt[18] = poss;
    pkt[19] = ((sa & 0x0F) << 4) | (sb & 0x0F);
}

// ---------------------------------------------------------------------------
// Simulation state
// ---------------------------------------------------------------------------
typedef enum {
    PLAY_NORMAL,
    PLAY_GOAL_PAUSE,
} PlayState;

typedef struct {
    float     x, y, vx, vy;
    float     speed_kmh;
    uint8_t   score_a, score_b;
    uint8_t   poss;
    uint32_t  tick;
    PlayState state;
    int       pause_ticks;
    int       shot_cooldown;
} Sim;

static float randf(float min, float max) {
    return min + ((float)rand() / RAND_MAX) * (max - min);
}

static int randint(int min, int max) {
    return min + rand() % (max - min + 1);
}

static void sim_init(Sim *s) {
    memset(s, 0, sizeof(*s));
    s->x = FIELD_W / 2.0f;
    s->y = FIELD_H / 2.0f;
    s->vx = randf(10, 18) * (rand() % 2 ? 1 : -1);
    s->vy = randf(-6, 6);
    s->state = PLAY_NORMAL;
}

static void sim_kickoff(Sim *s, int direction) {
    s->x = FIELD_W / 2.0f;
    s->y = FIELD_H / 2.0f + randf(-50, 50);
    s->vx = randf(12, 20) * direction;
    s->vy = randf(-8, 8);
    s->state = PLAY_NORMAL;
    s->shot_cooldown = 0;
}

static void sim_step(Sim *s) {
    s->tick++;

    // Pause after goal
    if (s->state == PLAY_GOAL_PAUSE) {
        s->pause_ticks--;
        if (s->pause_ticks <= 0) {
            // Kickoff towards player who just got scored on
            int dir = (s->score_a > s->score_b) ? -1 : 1;
            sim_kickoff(s, dir);
        }
        s->speed_kmh = 0;
        return;
    }

    float px = s->x, py = s->y;

    // Occasional "shot" — sudden speed boost
    if (s->shot_cooldown <= 0 && rand() % 40 == 0) {
        float boost = randf(1.8f, 2.5f);
        s->vx *= boost;
        s->vy *= boost;
        // Cap top speed
        float spd = sqrtf(s->vx*s->vx + s->vy*s->vy);
        if (spd > 50.0f) { s->vx *= 50.0f/spd; s->vy *= 50.0f/spd; }
        s->shot_cooldown = 15;
    }
    if (s->shot_cooldown > 0) s->shot_cooldown--;

    // Friction (slight slowdown)
    s->vx *= 0.985f;
    s->vy *= 0.985f;

    // Random small kicks from "players" hitting the ball
    if (s->tick % randint(8, 20) == 0) {
        s->vx += randf(-3, 3);
        s->vy += randf(-2, 2);
    }

    // Speed floor — keep ball moving
    float spd = sqrtf(s->vx*s->vx + s->vy*s->vy);
    if (spd < 8.0f) {
        float sign_x = (s->vx >= 0) ? 1 : -1;
        s->vx = sign_x * randf(8, 14);
        s->vy = randf(-5, 5);
    }

    s->x += s->vx;
    s->y += s->vy;

    // Bounce off top/bottom walls
    if (s->y <= 0) {
        s->vy = fabsf(s->vy);
        s->y = 1;
    }
    if (s->y >= FIELD_H) {
        s->vy = -fabsf(s->vy);
        s->y = FIELD_H - 1;
    }

    // Goal — left wall = team B scores
    if (s->x <= 0) {
        s->score_b = (s->score_b + 1) % 10;
        printf("[SIM] GOAL B! %d-%d\n", s->score_a, s->score_b);
        s->state = PLAY_GOAL_PAUSE;
        s->pause_ticks = 15;  // ~1.5s pause
        s->x = 0;
        return;
    }
    // Goal — right wall = team A scores
    if (s->x >= FIELD_W) {
        s->score_a = (s->score_a + 1) % 10;
        printf("[SIM] GOAL A! %d-%d\n", s->score_a, s->score_b);
        s->state = PLAY_GOAL_PAUSE;
        s->pause_ticks = 15;
        s->x = FIELD_W;
        return;
    }

    // Speed in km/h — distance per 100ms tick
    float dx = s->x - px;
    float dy = s->y - py;
    float dist_mm = sqrtf(dx*dx + dy*dy);
    s->speed_kmh = dist_mm * 0.036f;  // mm/100ms -> km/h

    // Possession from ball X position
    s->poss = (s->x < FIELD_W / 2.0f) ? 1 : 2;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Ball Tracking Simulator ===\n");

    // Seed RNG with adc readings — varies between boots
    srand(time_us_32());

    i2c_init(I2C_PORT, 9600);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    Sim sim;
    sim_init(&sim);

    uint8_t  pkt[PACKET_SIZE];
    uint32_t sent  = 0;
    uint32_t nacks = 0;

    while (true) {
        sim_step(&sim);

        build_packet(pkt,
                     (uint16_t)sim.x, (uint16_t)sim.y,
                     (uint16_t)(sim.x - sim.vx),
                     (uint16_t)(sim.y - sim.vy),
                     sim.speed_kmh,
                     sim.poss,
                     sim.score_a, sim.score_b);

        int r = i2c_write_timeout_us(I2C_PORT, SLAVE_ADDR, pkt,
                                      PACKET_SIZE, false, 100000);
        sent++;
        if (r != PACKET_SIZE) nacks++;

        if (sent % 20 == 0) {
            printf("[SIM #%lu] x=%4d y=%3d spd=%5.1f km/h  "
                   "%s  %d-%d  %s (nacks=%lu)\n",
                   sent, (int)sim.x, (int)sim.y,
                   sim.speed_kmh,
                   sim.poss == 1 ? "A" : "B",
                   sim.score_a, sim.score_b,
                   sim.state == PLAY_GOAL_PAUSE ? "GOAL!" : "play",
                   nacks);
        }

        sleep_ms(100);  // 10 Hz
    }
}
