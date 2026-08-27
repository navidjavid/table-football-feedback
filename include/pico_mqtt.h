#pragma once
#include <stdbool.h>
#include <stdint.h>

// Thin wrapper around lwIP's raw (NO_SYS) MQTT client: connects (with a
// Last-Will-Testament so the server detects a drop immediately), publishes
// the four outbound message shapes, and subscribes to the two messages the
// Pi sends back for this Pico/table (authoritative player identity, and
// game-state sync). Callbacks fire synchronously out of lwIP's TCP receive
// path, i.e. from inside cyw43_arch_poll() — already called every main-loop
// iteration — so no separate poll function is needed here.

void mqtt_app_init(const char *broker_ip, uint16_t port,
                    const char *client_id, int table_id);
bool mqtt_app_connected(void);

// Authoritative player identity for a tap, echoed back by the Pi in
// response to a published /rfid event: tablefootball/pico/<pico_id>/player
// name/side are only valid while seated == true.
typedef void (*mqtt_player_cb_t)(const char *name, const char *side, bool seated);
void mqtt_app_on_player(mqtt_player_cb_t cb);

// One roster slot as reported in a table `sync` snapshot.
typedef struct {
    bool filled;
    char name[24];
    char uid_hex[16];
} MqttSyncPlayer;

// Authoritative game-state snapshot pushed by the Pi (retained), including
// each side's full roster: tablefootball/table/<table_id>/sync
//
// This is the ONLY channel that carries admin-driven registration changes
// (add/remove player from the dashboard, or a forced admin "start") —
// those produce no RFID tap at all, so neither board's tap-driven local
// logic would otherwise ever learn about them. team_a/team_b are always
// exactly MAX_PLAYERS_PER_SIDE (2) entries, index 0 = slot 1.
typedef void (*mqtt_sync_cb_t)(const char *state, int score_a, int score_b,
                               const MqttSyncPlayer team_a[2],
                               const MqttSyncPlayer team_b[2]);
void mqtt_app_on_sync(mqtt_sync_cb_t cb);

// A raw RFID tap seen on the table-wide (not per-Pico) topic:
// tablefootball/table/<table_id>/rfid — carries every tap published by
// EITHER side's board, including our own (echoed back to us since we're
// also a subscriber). This is how one side's board learns the other
// side's roster without waiting on a server round-trip.
typedef void (*mqtt_rfid_cb_t)(const char *side, int slot, const char *uid_hex);
void mqtt_app_on_rfid(mqtt_rfid_cb_t cb);

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
