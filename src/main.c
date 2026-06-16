#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "rfid_handler.h"
#include "i2c_comms.h"
#include "game_logic.h"
#include "display_manager.h"

// --- Config ---
#define WIFI_SSID      "Navid's A55"
#define WIFI_PASSWORD  "zxc123456"
#define SERVER_IP      "10.27.62.46"
#define SERVER_PORT    5000

// ---------------------------------------------------------------------------
// HTTP POST — lwIP raw API (polling mode)
// ---------------------------------------------------------------------------
static bool _http_busy = false;

typedef struct {
    char body[512];
} HttpPost;

static err_t _http_recv(void *arg, struct tcp_pcb *pcb,
                         struct pbuf *pb, err_t err) {
    if (pb) pbuf_free(pb);
    tcp_close(pcb);
    _http_busy = false;
    return ERR_OK;
}

static void _http_err(void *arg, err_t err) {
    (void)arg; (void)err;
    _http_busy = false;
}

static err_t _http_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    if (err != ERR_OK) {
        _http_busy = false;
        return err;
    }

    HttpPost *p = (HttpPost *)arg;
    char request[800];
    int  body_len = strlen(p->body);

    snprintf(request, sizeof(request),
             "POST /update HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             SERVER_IP, SERVER_PORT, body_len, p->body);

    tcp_write(pcb, request, strlen(request), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void send_state(const GameData *g) {
    if (_http_busy) return;

    static HttpPost post;
    snprintf(post.body, sizeof(post.body),
             "{\"state\":\"%s\","
             "\"p1\":\"%s\",\"p2\":\"%s\","
             "\"score_a\":%d,\"score_b\":%d,"
             "\"speed\":%.1f,\"fastest\":%.1f,"
             "\"possession\":%d,"
             "\"ball_x\":%d,\"ball_y\":%d,"
             "\"time\":%lu}",
             game_state_label(g->state),
             g->p1_name, g->p2_name,
             g->score_a, g->score_b,
             g->current_kmh, g->fastest_kmh,
             g->possession,
             g->ball_x, g->ball_y,
             game_elapsed_seconds(g));

    ip_addr_t ip;
    if (!ipaddr_aton(SERVER_IP, &ip)) {
        printf("[HTTP] Invalid server IP\n");
        return;
    }

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) return;

    _http_busy = true;
    tcp_arg(pcb,  &post);
    tcp_recv(pcb, _http_recv);
    tcp_err(pcb,  _http_err);

    err_t err = tcp_connect(pcb, &ip, SERVER_PORT, _http_connected);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        _http_busy = false;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Table Football Feedback ===\n");

    // --- WiFi init ---
    if (cyw43_arch_init()) {
        printf("[WiFi] init FAILED\n");
    } else {
        cyw43_arch_enable_sta_mode();
        printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);

        if (cyw43_arch_wifi_connect_timeout_ms(
                WIFI_SSID, WIFI_PASSWORD,
                CYW43_AUTH_WPA2_AES_PSK, 20000)) {
            printf("[WiFi] Connection FAILED\n");
        } else {
            printf("[WiFi] Connected! IP: %s\n",
                   ip4addr_ntoa(netif_ip4_addr(netif_list)));
        }
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
    uint32_t  last_send  = 0;
    uint32_t  last_log   = 0;

    while (true) {
        loop++;
        cyw43_arch_poll();  // keep WiFi stack alive

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // --- State transition ---
        if (game.state != prev_state) {
            printf("[STATE] %s -> %s\n",
                   game_state_label(prev_state),
                   game_state_label(game.state));
            if (game.state == GAME_PLAYING) {
                i2c_comms_flush();
                printf("[I2C] Buffer flushed\n");
            }
            prev_state = game.state;
        }

        // --- I2C ---
        i2c_comms_poll(&fresh);
        if (fresh.valid) {
            ball = fresh;
            game_update(&game, &ball);
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
            game_register_player(&game, rfid.uid, name);
        }

        // --- Display every 250ms ---
        if (loop % 3 == 0)
            display_manager_render(&game);

        // --- Send to Pi every second ---
        if (now - last_send > 1000) {
            last_send = now;
            send_state(&game);
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
