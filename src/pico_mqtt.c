#include "pico_mqtt.h"
#include "pico/stdlib.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include <stdio.h>
#include <string.h>

static mqtt_client_t *_client;
static ip_addr_t      _broker_ip;
static uint16_t       _broker_port;
static char           _client_id[32];

// Track our own view of the connection state so we never call
// mqtt_client_connect() while a previous attempt is still in flight
// (lwIP returns ERR_ISCONN when conn_state != TCP_DISCONNECTED).
typedef enum { LINK_IDLE, LINK_CONNECTING, LINK_UP } LinkState;
static LinkState _link = LINK_IDLE;
static uint32_t  _last_attempt_ms = 0;
#define RECONNECT_BACKOFF_MS  5000

static uint32_t _now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void _connection_cb(mqtt_client_t *client, void *arg,
                           mqtt_connection_status_t status) {
    (void)client; (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        _link = LINK_UP;
        printf("[MQTT] Connected to broker\n");
    } else {
        _link = LINK_IDLE;
        printf("[MQTT] Connection failed/closed (status=%d)\n", status);
    }
}

static void _pub_request_cb(void *arg, err_t result) {
    (void)arg;
    if (result != ERR_OK) {
        printf("[MQTT] Publish failed err=%d\n", result);
    }
}

static void _connect(void) {
    // Don't start a second connect while one is already pending.
    if (_link == LINK_CONNECTING || _link == LINK_UP) return;

    uint32_t now = _now_ms();
    if (_last_attempt_ms != 0 && now - _last_attempt_ms < RECONNECT_BACKOFF_MS) return;
    _last_attempt_ms = now;

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id  = _client_id;
    ci.keep_alive = 30;

    _link = LINK_CONNECTING;
    err_t err = mqtt_client_connect(_client, &_broker_ip, _broker_port,
                                    _connection_cb, NULL, &ci);
    if (err != ERR_OK) {
        _link = LINK_IDLE;
        printf("[MQTT] connect() failed err=%d\n", err);
    } else {
        printf("[MQTT] Connecting to %s:%d ...\n",
               ipaddr_ntoa(&_broker_ip), _broker_port);
    }
}

void mqtt_app_init(const char *broker_ip, uint16_t port, const char *client_id) {
    if (!ipaddr_aton(broker_ip, &_broker_ip)) {
        printf("[MQTT] invalid broker ip '%s'\n", broker_ip);
        return;
    }
    _broker_port = port;
    strncpy(_client_id, client_id, sizeof(_client_id) - 1);
    _client_id[sizeof(_client_id) - 1] = '\0';

    _client = mqtt_client_new();
    if (!_client) {
        printf("[MQTT] mqtt_client_new() failed\n");
        return;
    }
    _connect();
}

bool mqtt_app_connected(void) {
    return _link == LINK_UP && _client && mqtt_client_is_connected(_client);
}

// If lwIP has closed the underlying TCP connection behind our back,
// fall back to LINK_IDLE so the next reconnect attempt is allowed.
static void _ensure_link_state_synced(void) {
    if (!_client) return;
    if (_link == LINK_UP && !mqtt_client_is_connected(_client)) {
        _link = LINK_IDLE;
    }
}

static void _publish(const char *topic, const char *payload, uint8_t qos) {
    _ensure_link_state_synced();
    if (_link != LINK_UP) {
        // Try to reconnect at most once per RECONNECT_BACKOFF_MS; drop
        // this publish rather than queuing.
        _connect();
        return;
    }
    err_t err = mqtt_publish(_client, topic, payload, (uint16_t)strlen(payload),
                             qos, 0, _pub_request_cb, NULL);
    if (err != ERR_OK) {
        printf("[MQTT] publish() queue failed err=%d (topic=%s)\n", err, topic);
    }
}

void mqtt_publish_heartbeat(const char *pico_id, int table_id,
                            const char *side, const char *role,
                            const char *ip, const char *firmware,
                            uint32_t uptime_s) {
    char topic[64], body[256];
    snprintf(topic, sizeof(topic), "tablefootball/pico/%s/heartbeat", pico_id);
    snprintf(body, sizeof(body),
             "{\"v\":1,\"pico_id\":\"%s\",\"table_id\":%d,\"side\":\"%s\","
             "\"role\":\"%s\",\"ip\":\"%s\",\"firmware\":\"%s\",\"uptime\":%lu}",
             pico_id, table_id, side, role, ip, firmware,
             (unsigned long)uptime_s);
    _publish(topic, body, 0);
}

void mqtt_publish_rfid(int table_id, const char *pico_id,
                       const char *side, int slot, const char *uid_hex) {
    char topic[48], body[160];
    snprintf(topic, sizeof(topic), "tablefootball/table/%d/rfid", table_id);
    snprintf(body, sizeof(body),
             "{\"v\":1,\"pico_id\":\"%s\",\"table_id\":%d,\"side\":\"%s\","
             "\"slot\":%d,\"uid\":\"%s\",\"event\":\"card_tapped\"}",
             pico_id, table_id, side, slot, uid_hex);
    _publish(topic, body, 1);
}

void mqtt_publish_state(int table_id, const char *pico_id,
                        const char *session_id, const char *state,
                        const char *mode, int score_a, int score_b,
                        float fastest, const char *winner_side,
                        const char *winner_uid, uint32_t time_s) {
    char topic[48], body[320];
    snprintf(topic, sizeof(topic), "tablefootball/table/%d/state", table_id);
    snprintf(body, sizeof(body),
             "{\"v\":1,\"pico_id\":\"%s\",\"table_id\":%d,\"session_id\":\"%s\","
             "\"state\":\"%s\",\"mode\":\"%s\",\"score_a\":%d,\"score_b\":%d,"
             "\"fastest\":%.1f,\"winner_side\":\"%s\",\"winner_uid\":\"%s\","
             "\"time\":%lu}",
             pico_id, table_id, session_id, state, mode, score_a, score_b,
             fastest, winner_side ? winner_side : "",
             winner_uid ? winner_uid : "", (unsigned long)time_s);
    _publish(topic, body, 1);
}

void mqtt_publish_ball(int table_id, float x, float y, float speed) {
    char topic[48], body[128];
    snprintf(topic, sizeof(topic), "tablefootball/table/%d/ball", table_id);
    snprintf(body, sizeof(body),
             "{\"v\":1,\"table_id\":%d,\"x\":%.1f,\"y\":%.1f,\"speed\":%.1f}",
             table_id, x, y, speed);
    _publish(topic, body, 0);
}
