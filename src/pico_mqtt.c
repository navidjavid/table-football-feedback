#include "pico_mqtt.h"
#include "pico/stdlib.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mqtt_client_t *_client;
static ip_addr_t      _broker_ip;
static uint16_t       _broker_port;
static char           _client_id[32];

// Built once in mqtt_app_init() from pico_id/table_id.
static char _will_topic[48];
static char _player_topic[48];
static char _sync_topic[48];
static char _rfid_topic[48];
static const char _will_msg[] = "{\"online\":false}";

static mqtt_player_cb_t _player_cb;
static mqtt_sync_cb_t   _sync_cb;
static mqtt_rfid_cb_t   _rfid_cb;

// Incoming-publish reassembly buffer. lwIP delivers a subscribed
// message's payload in one or more chunks via _incoming_data_cb();
// _incoming_publish_cb() tells us which topic the next chunks belong to.
// Sized for the worst case: a 2v2 `sync` snapshot carries full team_a/
// team_b rosters (name+uid for up to 4 players) — comfortably over the
// 256B this used to be sized for back when sync was just state+score.
#define RX_BUF_SIZE 512
static char   _rx_topic[48];
static char   _rx_buf[RX_BUF_SIZE];
static size_t _rx_len;

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

// ---------------------------------------------------------------------------
// Minimal flat-JSON field extraction. Our incoming payloads are always a
// single-level object with known key names, so this deliberately doesn't
// need a real JSON parser.
// ---------------------------------------------------------------------------
static bool _json_str(const char *json, const char *key, char *out, size_t out_sz) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t n = (size_t)(end - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool _json_bool(const char *json, const char *key, bool *out) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    *out = (strncmp(p, "true", 4) == 0);
    return true;
}

static bool _json_int(const char *json, const char *key, int *out) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    *out = atoi(p + strlen(pat));
    return true;
}

// Parses a flat array of flat objects, e.g.
//   "team_a":[{"slot":1,"name":"Alice","uid":"DBEF7005",...}, ...]
// into up to 2 slots (index 0 = slot 1). Objects here never nest further,
// so "first matching bracket" is always the right one — no real JSON
// parser needed, same spirit as the helpers above.
static void _parse_team(const char *json, const char *key, MqttSyncPlayer out[2]) {
    out[0].filled = false; out[0].name[0] = '\0'; out[0].uid_hex[0] = '\0';
    out[1].filled = false; out[1].name[0] = '\0'; out[1].uid_hex[0] = '\0';

    char pat[16];
    snprintf(pat, sizeof(pat), "\"%s\":[", key);
    const char *arr = strstr(json, pat);
    if (!arr) return;
    arr += strlen(pat);
    const char *arr_end = strchr(arr, ']');
    if (!arr_end) return;

    const char *p = arr;
    while (p < arr_end) {
        const char *obj_start = memchr(p, '{', (size_t)(arr_end - p));
        if (!obj_start) break;
        const char *obj_end = memchr(obj_start, '}', (size_t)(arr_end - obj_start));
        if (!obj_end) break;

        char obj[128];
        size_t n = (size_t)(obj_end - obj_start) + 1;
        if (n >= sizeof(obj)) n = sizeof(obj) - 1;
        memcpy(obj, obj_start, n);
        obj[n] = '\0';

        int slot = 0;
        _json_int(obj, "slot", &slot);
        if (slot >= 1 && slot <= 2) {
            MqttSyncPlayer *s = &out[slot - 1];
            char name[24] = "";
            if (_json_str(obj, "name", name, sizeof(name)) && name[0]) {
                strncpy(s->name, name, sizeof(s->name) - 1);
                s->name[sizeof(s->name) - 1] = '\0';
                s->filled = true;
            }
            _json_str(obj, "uid", s->uid_hex, sizeof(s->uid_hex));
        }
        p = obj_end + 1;
    }
}

// ---------------------------------------------------------------------------
// Incoming publish handling (subscribed topics only)
// ---------------------------------------------------------------------------
static void _incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    (void)arg;
    strncpy(_rx_topic, topic, sizeof(_rx_topic) - 1);
    _rx_topic[sizeof(_rx_topic) - 1] = '\0';
    _rx_len = 0;
    if (tot_len >= RX_BUF_SIZE) {
        printf("[MQTT] incoming payload too large (%lu bytes) on %s\n",
               (unsigned long)tot_len, topic);
    }
}

static void _route_incoming(const char *topic, const char *payload) {
    if (strcmp(topic, _player_topic) == 0) {
        if (!_player_cb) return;
        char name[24] = "";
        char side[4]  = "";
        bool seated   = false;
        _json_str(payload, "name", name, sizeof(name));
        _json_str(payload, "side", side, sizeof(side));
        _json_bool(payload, "seated", &seated);
        _player_cb(name, side, seated);
    } else if (strcmp(topic, _sync_topic) == 0) {
        if (!_sync_cb) return;
        char state[16] = "";
        int score_a = 0, score_b = 0;
        _json_str(payload, "state", state, sizeof(state));
        _json_int(payload, "score_a", &score_a);
        _json_int(payload, "score_b", &score_b);
        MqttSyncPlayer team_a[2], team_b[2];
        _parse_team(payload, "team_a", team_a);
        _parse_team(payload, "team_b", team_b);
        _sync_cb(state, score_a, score_b, team_a, team_b);
    } else if (strcmp(topic, _rfid_topic) == 0) {
        if (!_rfid_cb) return;
        char side[4] = "";
        char uid[16] = "";
        int slot = 0;
        _json_str(payload, "side", side, sizeof(side));
        _json_str(payload, "uid", uid, sizeof(uid));
        _json_int(payload, "slot", &slot);
        _rfid_cb(side, slot, uid);
    }
}

static void _incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg;
    size_t space = (RX_BUF_SIZE - 1) - _rx_len;
    size_t n = (len < space) ? len : space;
    memcpy(&_rx_buf[_rx_len], data, n);
    _rx_len += n;

    if (flags & MQTT_DATA_FLAG_LAST) {
        _rx_buf[_rx_len] = '\0';
        _route_incoming(_rx_topic, _rx_buf);
        _rx_len = 0;
    }
}

static void _sub_request_cb(void *arg, err_t err) {
    const char *topic = (const char *)arg;
    if (err != ERR_OK) {
        printf("[MQTT] subscribe to %s failed err=%d\n", topic, err);
    }
}

static void _subscribe_all(void) {
    mqtt_subscribe(_client, _player_topic, 1, _sub_request_cb, (void *)_player_topic);
    mqtt_subscribe(_client, _sync_topic,   1, _sub_request_cb, (void *)_sync_topic);
    mqtt_subscribe(_client, _rfid_topic,   1, _sub_request_cb, (void *)_rfid_topic);
}

static void _connection_cb(mqtt_client_t *client, void *arg,
                           mqtt_connection_status_t status) {
    (void)client; (void)arg;
    if (status == MQTT_CONNECT_ACCEPTED) {
        _link = LINK_UP;
        printf("[MQTT] Connected to broker\n");
        _subscribe_all(); // subscriptions don't survive a reconnect
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
    // No client yet — mqtt_app_init() was either never called (e.g. WiFi
    // never came up at boot, so main.c skipped it entirely) or failed to
    // allocate one. Nothing to connect; every publish path must be able
    // to call this safely and just no-op in that case.
    if (!_client) return;

    // Don't start a second connect while one is already pending.
    if (_link == LINK_CONNECTING || _link == LINK_UP) return;

    uint32_t now = _now_ms();
    if (_last_attempt_ms != 0 && now - _last_attempt_ms < RECONNECT_BACKOFF_MS) return;
    _last_attempt_ms = now;

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id   = _client_id;
    ci.keep_alive  = 30;
    ci.will_topic  = _will_topic;
    ci.will_msg    = _will_msg;
    ci.will_qos    = 1;
    ci.will_retain = 1; // so a client subscribing after we drop still sees "offline"

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

void mqtt_app_init(const char *broker_ip, uint16_t port,
                    const char *client_id, int table_id) {
    if (!ipaddr_aton(broker_ip, &_broker_ip)) {
        printf("[MQTT] invalid broker ip '%s'\n", broker_ip);
        return;
    }
    _broker_port = port;
    strncpy(_client_id, client_id, sizeof(_client_id) - 1);
    _client_id[sizeof(_client_id) - 1] = '\0';

    snprintf(_will_topic, sizeof(_will_topic), "tablefootball/pico/%s/status", client_id);
    snprintf(_player_topic, sizeof(_player_topic), "tablefootball/pico/%s/player", client_id);
    snprintf(_sync_topic, sizeof(_sync_topic), "tablefootball/table/%d/sync", table_id);
    snprintf(_rfid_topic, sizeof(_rfid_topic), "tablefootball/table/%d/rfid", table_id);

    _client = mqtt_client_new();
    if (!_client) {
        printf("[MQTT] mqtt_client_new() failed\n");
        return;
    }
    mqtt_set_inpub_callback(_client, _incoming_publish_cb, _incoming_data_cb, NULL);
    _connect();
}

bool mqtt_app_connected(void) {
    return _link == LINK_UP && _client && mqtt_client_is_connected(_client);
}

void mqtt_app_on_player(mqtt_player_cb_t cb) { _player_cb = cb; }
void mqtt_app_on_sync(mqtt_sync_cb_t cb)     { _sync_cb = cb; }
void mqtt_app_on_rfid(mqtt_rfid_cb_t cb)     { _rfid_cb = cb; }

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
