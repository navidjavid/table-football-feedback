#pragma once
#include <stdbool.h>
#include <stdint.h>

// Thin wrapper around lwIP's raw (NO_SYS) MQTT client, scoped to exactly
// what this single-Pico demo needs: connect once, publish four message
// shapes. No subscriptions — the Pico doesn't need anything back from
// the Pi for this demo.

void mqtt_app_init(const char *broker_ip, uint16_t port, const char *client_id);
bool mqtt_app_connected(void);

// tablefootball/pico/<pico_id>/heartbeat
void mqtt_publish_heartbeat(const char *pico_id, int table_id,
                             const char *side, const char *role,
                             const char *ip, const char *firmware,
                             uint32_t uptime_s);

// tablefootball/table/<table_id>/rfid
void mqtt_publish_rfid(int table_id, const char *pico_id,
                       const char *side, int slot, const char *uid_hex);

// tablefootball/table/<table_id>/state
void mqtt_publish_state(int table_id, const char *pico_id,
                        const char *session_id, const char *state,
                        const char *mode, int score_a, int score_b,
                        float fastest, const char *winner_side,
                        const char *winner_uid, uint32_t time_s);

// tablefootball/table/<table_id>/ball  (QoS 0, high frequency)
void mqtt_publish_ball(int table_id, float x, float y, float speed);
