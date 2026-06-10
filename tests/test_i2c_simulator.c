/**
 * test_i2c_simulator.c — Realistic foosball ball tracker simulator
 *
 * Goal frequency: ~every 20-60 seconds (realistic)
 * Ball movement: natural bouncing with player deflections
 * Speed: 2-25 km/h range with occasional fast shots
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
                          uint16_t x, uint16_t y,
                          uint16_t px, uint16_t py,
                          float speed, uint8_t poss,
                          uint8_t sa, uint8_t sb) {
    pkt[0]  = SYNC_A; pkt[1] = SYNC_B;
    pkt[2]  = (x>>8)&0xFF;  pkt[3]  = x&0xFF;
    pkt[4]  = (y>>8)&0xFF;  pkt[5]  = y&0xFF;
    pkt[6]  = (px>>8)&0xFF; pkt[7]  = px&0xFF;
    pkt[8]  = (py>>8)&0xFF; pkt[9]  = py&0xFF;
    pkt[10] = (FIELD_W>>8)&0xFF; pkt[11] = FIELD_W&0xFF;
    pkt[12] = (FIELD_H>>8)&0xFF; pkt[13] = FIELD_H&0xFF;
    pack_float(&pkt[14], speed);
    pkt[18] = poss;
    pkt[19] = ((sa&0x0F)<<4) | (sb&0x0F);
}

// ---------------------------------------------------------------------------
// Realistic ball physics
// ---------------------------------------------------------------------------

// 4 "player" rod positions on each half
// Rods deflect the ball back towards the opponent's goal
static const float rod_positions[] = {
    100, 250, 400, 600, 750, 900  // x positions of "rods"
};
#define NUM_RODS  6

typedef enum { PLAY, GOAL_PAUSE, POSSESSION_HOLD } SimState;

typedef struct {
    float    x, y, vx, vy;
    float    speed_kmh;
    uint8_t  score_a, score_b;
    uint8_t  poss;
    uint32_t tick;
    SimState state;
    int      pause_ticks;
    int      hold_ticks;        // ball held by player (slow down)
} Sim;

static float randf(float lo, float hi) {
    return lo + ((float)rand() / RAND_MAX) * (hi - lo);
}

static void sim_init(Sim *s) {
    memset(s, 0, sizeof(*s));
    s->x  = FIELD_W / 2.0f;
    s->y  = FIELD_H / 2.0f;
    s->vx = randf(3, 6) * (rand()%2 ? 1 : -1);
    s->vy = randf(-2, 2);
}

static void sim_kickoff(Sim *s, int dir) {
    s->x  = FIELD_W / 2.0f;
    s->y  = FIELD_H / 2.0f + randf(-80, 80);
    s->vx = randf(4, 7) * dir;
    s->vy = randf(-3, 3);
    s->state = PLAY;
    s->hold_ticks = 0;
}

static void sim_step(Sim *s) {
    s->tick++;

    // --- Goal pause ---
    if (s->state == GOAL_PAUSE) {
        s->pause_ticks--;
        s->speed_kmh = 0;
        if (s->pause_ticks <= 0)
            sim_kickoff(s, (rand()%2) ? 1 : -1);
        return;
    }

    // --- Ball held by player (slowed, then kicked) ---
    if (s->state == POSSESSION_HOLD) {
        s->hold_ticks--;
        s->vx *= 0.8f;  // slow down during hold
        s->vy *= 0.8f;
        s->speed_kmh = sqrtf(s->vx*s->vx + s->vy*s->vy) * 0.036f;
        if (s->hold_ticks <= 0) {
            // Kick! Random direction towards opponent goal
            float dir = (s->x < FIELD_W/2) ? 1.0f : -1.0f;
            float power = randf(5, 12);
            s->vx = dir * power;
            s->vy = randf(-4, 4);
            s->state = PLAY;
        }
        s->x += s->vx;
        s->y += s->vy;
        s->poss = (s->x < FIELD_W/2) ? 1 : 2;
        return;
    }

    // --- Normal play ---
    float px = s->x, py = s->y;

    // Friction — natural slowdown
    s->vx *= 0.995f;
    s->vy *= 0.995f;

    // Random rod deflection — simulates player interaction
    // Check if ball passes near a rod position
    for (int i = 0; i < NUM_RODS; i++) {
        float dx = s->x - rod_positions[i];
        if (fabsf(dx) < 15) {
            // ~15% chance of deflection per tick near a rod
            if (rand() % 100 < 3) {
                // Player "hits" the ball — change direction
                s->vx = randf(3, 8) * ((s->x < FIELD_W/2) ? 1 : -1);
                s->vy = randf(-4, 4);
                break;
            }
            // ~5% chance of player "holding" the ball
            if (rand() % 100 < 1) {
                s->state = POSSESSION_HOLD;
                s->hold_ticks = (int)randf(10, 30);  // 1-3 seconds hold
                break;
            }
        }
    }

    // Occasional power shot (~0.5% per tick)
    if (rand() % 200 == 0) {
        float dir = (s->x < FIELD_W/2) ? 1.0f : -1.0f;
        s->vx = dir * randf(12, 20);
        s->vy = randf(-3, 3);
    }

    // Speed floor — ball should always be moving somewhat
    float spd = sqrtf(s->vx*s->vx + s->vy*s->vy);
    if (spd < 2.0f) {
        s->vx = randf(3, 5) * ((rand()%2) ? 1 : -1);
        s->vy = randf(-2, 2);
    }

    s->x += s->vx;
    s->y += s->vy;

    // Wall bounce (top/bottom)
    if (s->y <= 0)        { s->vy = fabsf(s->vy); s->y = 1; }
    if (s->y >= FIELD_H)  { s->vy = -fabsf(s->vy); s->y = FIELD_H-1; }

    // Goal detection — only middle 60% of wall is the goal mouth
    int goal_top    = FIELD_H * 0.2f;
    int goal_bottom = FIELD_H * 0.8f;
    bool in_goal_zone = (s->y > goal_top && s->y < goal_bottom);

    if (s->x <= 0) {
        if (in_goal_zone) {
            s->score_b = (s->score_b + 1) % 10;
            s->state = GOAL_PAUSE;
            s->pause_ticks = 30;  // 3 seconds pause
            printf("[SIM] GOAL B! %d-%d\n", s->score_a, s->score_b);
        } else {
            s->vx = fabsf(s->vx) * 0.7f;
            s->x = 1;
        }
    }
    if (s->x >= FIELD_W) {
        if (in_goal_zone) {
            s->score_a = (s->score_a + 1) % 10;
            s->state = GOAL_PAUSE;
            s->pause_ticks = 30;
            printf("[SIM] GOAL A! %d-%d\n", s->score_a, s->score_b);
        } else {
            s->vx = -fabsf(s->vx) * 0.7f;
            s->x = FIELD_W - 1;
        }
    }

    // Speed in km/h
    float ddx = s->x - px, ddy = s->y - py;
    s->speed_kmh = sqrtf(ddx*ddx + ddy*ddy) * 0.036f;
    s->poss = (s->x < FIELD_W / 2.0f) ? 1 : 2;
}

// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Realistic Foosball Simulator ===\n");

    srand(time_us_32());

    i2c_init(I2C_PORT, 9600);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);

    Sim sim;
    sim_init(&sim);

    uint8_t  pkt[PACKET_SIZE];
    uint32_t sent = 0;

    while (true) {
        sim_step(&sim);

        build_packet(pkt,
                     (uint16_t)sim.x, (uint16_t)sim.y,
                     (uint16_t)(sim.x - sim.vx),
                     (uint16_t)(sim.y - sim.vy),
                     sim.speed_kmh, sim.poss,
                     sim.score_a, sim.score_b);

        i2c_write_timeout_us(I2C_PORT, SLAVE_ADDR, pkt,
                              PACKET_SIZE, false, 100000);
        sent++;

        if (sent % 50 == 0) {
            printf("[SIM] x=%4d y=%3d %5.1fkm/h %s %d-%d %s\n",
                   (int)sim.x, (int)sim.y, sim.speed_kmh,
                   sim.poss==1?"A":"B", sim.score_a, sim.score_b,
                   sim.state==GOAL_PAUSE?"GOAL!":
                   sim.state==POSSESSION_HOLD?"HOLD":"play");
        }

        sleep_ms(100);
    }
}
