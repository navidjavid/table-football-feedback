#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "rfid_handler.h"
#include "i2c_comms.h"
#include "game_logic.h"
#include "display_manager.h"
#include "pico_mqtt.h"

// --- WiFi: join the Pi's hotspot ---
#define WIFI_SSID      "TableFootball"
#define WIFI_PASSWORD  "12345678"

// --- MQTT broker on the Pi itself ---
#define MQTT_BROKER_IP   "192.168.4.1"
#define MQTT_BROKER_PORT 1883

// --- This demo runs a single Pico handling both sides of Table 1 ---
#define PICO_ID    "pico-demo"
#define TABLE_ID   1
#define FIRMWARE   "demo-0.2.0"

static char _device_ip[16] = "0.0.0.0";
static char _session_id[40] = "";

static void uid_to_hex(const uint8_t uid[4], char *out /* >= 9 bytes */) {
    snprintf(out, 9, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Table Football Feedback ===\n");

    // --- WiFi init ---
    // cyw43_arch_wifi_connect_timeout_ms() occasionally reports a timeout
    // right as DHCP is still finishing (association can succeed on the
    // hotspot side a few seconds after the call gives up). Retry a few
    // times, and on a reported failure double-check the actual link/IP
    // state before believing it.
    bool wifi_ok = false;
    if (cyw43_arch_init()) {
        printf("[WiFi] init FAILED\n");
    } else {
        cyw43_arch_enable_sta_mode();

        for (int attempt = 1; attempt <= 3 && !wifi_ok; attempt++) {
            printf("[WiFi] Connecting to '%s' (attempt %d/3)...\n", WIFI_SSID, attempt);
            int rc = cyw43_arch_wifi_connect_timeout_ms(
                    WIFI_SSID, WIFI_PASSWORD,
                    CYW43_AUTH_WPA2_AES_PSK, 25000);

            int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            const ip4_addr_t *ip = netif_ip4_addr(netif_list);

            if (rc == 0 && link == CYW43_LINK_UP && !ip4_addr_isany(ip)) {
                wifi_ok = true;
            } else if (link == CYW43_LINK_UP && !ip4_addr_isany(ip)) {
                // Reported a timeout/error, but the link actually came up —
                // this is the late-DHCP race described above.
                printf("[WiFi] connect() reported rc=%d but link is up — treating as connected\n", rc);
                wifi_ok = true;
            } else {
                printf("[WiFi] Connection attempt failed (rc=%d), retrying...\n", rc);
                sleep_ms(1500);
            }
        }

        if (wifi_ok) {
            strncpy(_device_ip, ip4addr_ntoa(netif_ip4_addr(netif_list)),
                    sizeof(_device_ip) - 1);
            printf("[WiFi] Connected! IP: %s\n", _device_ip);
        } else {
            printf("[WiFi] Giving up after 3 attempts — running offline\n");
        }
    }

    // --- MQTT ---
    if (wifi_ok) {
        mqtt_app_init(MQTT_BROKER_IP, MQTT_BROKER_PORT, PICO_ID);
    }

    // --- Peripherals (display last — most timing-sensitive) ---
    rfid_handler_init();    printf("[init] RFID\n");
    i2c_comms_init();       printf("[init] I2C\n");
    display_manager_init(); printf("[init] Display\n");

    display_manager_show_splash();
    sleep_ms(1500);

    GameData  game;
    game_init(&game);

    BallData  ball       = {0};
    BallData  fresh      = {0};
    GameState prev_state = GAME_REGISTER_P1;
    uint32_t  loop       = 0;
    uint32_t  last_heartbeat = 0;
    uint32_t  last_state_pub = 0;
    uint32_t  last_log   = 0;
    uint32_t  boot_ms    = to_ms_since_boot(get_absolute_time());

    while (true) {
        loop++;
        cyw43_arch_poll();  // keep WiFi + lwIP (incl. MQTT) stack alive

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // --- State transition ---
        if (game.state != prev_state) {
            printf("[STATE] %s -> %s\n",
                   game_state_label(prev_state),
                   game_state_label(game.state));
            if (game.state == GAME_PLAYING) {
                i2c_comms_flush();
                printf("[I2C] Buffer flushed\n");
                snprintf(_session_id, sizeof(_session_id),
                        "%s-%lu", PICO_ID, (unsigned long)game.game_start_ms);
            }
            prev_state = game.state;
        }

        // --- I2C ---
        i2c_comms_poll(&fresh);
        if (fresh.valid) {
            ball = fresh;
            game_update(&game, &ball);

            // Ball position, scaled to the dashboard's 0-1000 x 0-500 field.
            if (game.state == GAME_PLAYING && game.field_w > 0 && game.field_h > 0) {
                float x = ((float)game.ball_x / game.field_w) * 1000.0f;
                float y = ((float)game.ball_y / game.field_h) * 500.0f;
                mqtt_publish_ball(TABLE_ID, x, y, game.current_kmh);
            }
        }

        // --- RFID ---
        RFIDScanResult rfid;
        rfid_handler_scan(&rfid);
        if (rfid.result == RFID_NEW_TAP) {
            const char *name = game_lookup_player(rfid.uid);
            printf("[RFID] %02X:%02X:%02X:%02X => %s\n",
                   rfid.uid[0], rfid.uid[1],
                   rfid.uid[2], rfid.uid[3],
                   name ? name : "Guest");

            // Side mapping for this single-Pico demo: first card -> side A
            // (slot 1), second card -> side B (slot 1). The Pi server owns
            // the actual registration/auto-start logic once it sees these
            // RFID taps; our local game_register_player() just mirrors it
            // for the on-cabinet display.
            // GAME_OVER's first tap starts a new P1 registration (see
            // game_register_player()'s GAME_OVER case), so it must map to
            // side A just like GAME_REGISTER_P1 — otherwise both taps of
            // a second game get published as side B and the table never
            // reaches GAME_PLAYING.
            const char *side =
                (game.state == GAME_REGISTER_P1 || game.state == GAME_OVER)
                ? "A" : "B";
            char uid_hex[9];
            uid_to_hex(rfid.uid, uid_hex);
            mqtt_publish_rfid(TABLE_ID, PICO_ID, side, 1, uid_hex);

            game_register_player(&game, rfid.uid, name);
        }

        // --- Display every 250ms ---
        if (loop % 3 == 0)
            display_manager_render(&game);

        // --- Heartbeat every 5s ---
        if (now - last_heartbeat > 5000) {
            last_heartbeat = now;
            mqtt_publish_heartbeat(PICO_ID, TABLE_ID, "A", "primary",
                                   _device_ip, FIRMWARE,
                                   (now - boot_ms) / 1000);
        }

        // --- Game state to Pi every second (only once there's a score to report) ---
        if ((game.state == GAME_PLAYING || game.state == GAME_OVER)
            && now - last_state_pub > 1000) {
            last_state_pub = now;

            const char *state_label =
                (game.state == GAME_OVER) ? "GAME_OVER" : "GAME_PLAYING";
            const char *winner_side = "";
            char winner_uid[9] = "";
            if (game.state == GAME_OVER) {
                winner_side = (game.winner == 1) ? "A" : "B";
                uid_to_hex(game.winner == 1 ? game.p1_uid : game.p2_uid, winner_uid);
            }

            mqtt_publish_state(TABLE_ID, PICO_ID, _session_id, state_label,
                               "1v1", game.score_a, game.score_b,
                               game.fastest_kmh, winner_side, winner_uid,
                               game_elapsed_seconds(&game));
        }

        // --- Serial log every 2s during play ---
        if (now - last_log > 2000) {
            last_log = now;
            if (game.state == GAME_PLAYING) {
                printf("[GAME] %s %d-%d %s  %.1fkm/h  t=%lus\n",
                       game.p1_name, game.score_a,
                       game.score_b, game.p2_name,
                       game.current_kmh,
                       game_elapsed_seconds(&game));
            }
        }

        sleep_ms(80);
    }
}
