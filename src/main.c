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

// ---------------------------------------------------------------------------
// Per-board config — final deployment is TWO independent PCBs, one per
// side (own Pico + RFID reader + display each), both reading the same
// shared ball-tracker I2C feed. Flip these three before flashing the
// OTHER side's board:
//   MY_SIDE            'A' on this board, 'B' on the other
//   PICO_ROLE_PRIMARY  1 on exactly ONE board (owns ball+state reporting
//                      to the Pi); 0 on the other (only reports its own
//                      side's RFID taps + heartbeat) — per the agreed
//                      split so the server never gets duplicate/racing
//                      ball or score data from both boards.
//   PICO_ID            must be unique per board
// ---------------------------------------------------------------------------
#define MY_SIDE            'A'
#define MY_SIDE_STR         "A"
#define PICO_ROLE_PRIMARY  1

#define PICO_ID    "pico-side-a"
#define TABLE_ID   1
#define FIRMWARE   "demo-0.3.0"

static char _device_ip[16] = "0.0.0.0";
static char _session_id[40] = "";

static void uid_to_hex(const uint8_t uid[4], char *out /* >= 9 bytes */) {
    snprintf(out, 9, "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
}

static bool hex_to_uid(const char *hex, uint8_t uid_out[4]) {
    if (strlen(hex) < 8) return false;
    for (int i = 0; i < 4; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        uid_out[i] = (uint8_t)byte;
    }
    return true;
}

static const char* local_mode_label(const GameData *g) {
    bool a2 = g->side_a[1].filled, b2 = g->side_b[1].filled;
    if (a2 && b2) return "2v2";
    if (!a2 && !b2) return "1v1";
    return "mixed";
}

// ---------------------------------------------------------------------------
// MQTT callbacks. All fire synchronously from cyw43_arch_poll(), same
// thread as main() — safe to touch GameData directly.
// ---------------------------------------------------------------------------
static GameData *_active_game = NULL;

// The Pi is the source of truth for player identity; this corrects our
// locally-guessed name (from the hardcoded demo roster in game_logic.c)
// once it resolves OUR OWN tap against its real database. Note: this only
// ever fires for taps THIS Pico published — the sibling board's players
// get their name corrected on the sibling board, not here.
static void on_pi_player(const char *name, const char *side, bool seated) {
    if (!_active_game || !seated || !name[0]) return;
    // Whichever of our own side's slots currently holds a name-mismatch
    // guess is unknowable from this message alone (it doesn't carry a
    // UID), so this only refines slot 1 — the common case for a fresh tap.
    if (side[0] != MY_SIDE) return;
    PlayerSlot *mine = (MY_SIDE == 'A') ? _active_game->side_a : _active_game->side_b;
    if (!mine[0].filled) return;
    strncpy(mine[0].name, name, NAME_LEN - 1);
    mine[0].name[NAME_LEN - 1] = '\0';
}

// The other side's board publishes its own RFID taps to this same
// table-scoped topic (we're a subscriber too, same as the server) — this
// is how each board learns the other side's roster without needing to
// wait for a server round-trip. Our own taps also arrive here (echoed
// back) and are ignored since we already applied them locally the
// instant the physical tap happened.
static void on_pi_rfid(const char *side, int slot, const char *uid_hex) {
    (void)slot;
    if (!_active_game || !side[0]) return;
    if (side[0] == MY_SIDE) return;

    uint8_t uid[4];
    if (!hex_to_uid(uid_hex, uid)) return;
    const char *name = game_lookup_player(uid);
    game_register_player(_active_game, side[0], uid, name);
}

// The local SCORE during active play comes from the real-time ball
// sensor (shared by both boards) and is already trusted for gameplay;
// forcibly overwriting it from a retained MQTT snapshot risks a visible
// rollback if the two ever briefly drift, so that part stays hands-off.
//
// Everything else about WHO is seated is only ever known for certain
// through this message. RFID taps only tell a board about players who
// tapped a physical reader; admin add_player/remove_player/start from
// the dashboard produce no RFID tap or ball event at all, so without
// this a board just sits showing stale/empty rosters no matter what the
// admin does. This is the one channel that reaches every case:
//   - admin "reset"/"stop"       -> server reports state=WAITING
//   - admin add/remove a player  -> server's team_a/team_b changes
//   - admin forces a "start"     -> server's state flips with no tap
static void on_pi_sync(const char *state, int score_a, int score_b,
                       const MqttSyncPlayer team_a[2], const MqttSyncPlayer team_b[2]) {
    printf("[MQTT] Pi sync: state=%s score=%d-%d\n", state, score_a, score_b);
    if (!_active_game) return;

    if (strcmp(state, "WAITING") == 0) {
        if (_active_game->state != GAME_WAITING) {
            printf("[MQTT] Pi says table is WAITING (admin reset/stop) -> resetting local game\n");
            game_init(_active_game);
        }
        return;
    }

    for (int slot = 1; slot <= MAX_PLAYERS_PER_SIDE; slot++) {
        const MqttSyncPlayer *a = &team_a[slot - 1];
        const MqttSyncPlayer *b = &team_b[slot - 1];

        uint8_t uid_a[4], uid_b[4];
        bool have_uid_a = a->filled && hex_to_uid(a->uid_hex, uid_a);
        bool have_uid_b = b->filled && hex_to_uid(b->uid_hex, uid_b);

        game_sync_roster_slot(_active_game, 'A', slot, a->filled, a->name,
                              have_uid_a ? uid_a : NULL);
        game_sync_roster_slot(_active_game, 'B', slot, b->filled, b->name,
                              have_uid_b ? uid_b : NULL);
    }
    // An admin-forced registration/start never comes with a local RFID
    // tap to trigger the usual WAITING -> PLAYING check, so do it here
    // too now that the roster above may have just become ready.
    game_recheck_start(_active_game);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Table Football Feedback (side %s, %s) ===\n",
           MY_SIDE_STR, PICO_ROLE_PRIMARY ? "primary" : "secondary");

    // Display is initialized first so boot progress is visible on the
    // cabinet screen, not just over USB serial. Every status write below
    // happens either before cyw43 starts at all, or in the gap between
    // (not during) a blocking WiFi call — the same "idle WiFi" timing
    // window the original code already relied on for the splash screen
    // that used to run right after WiFi finished. No write ever races an
    // in-progress connect attempt.
    display_manager_init(); printf("[init] Display\n");
    display_manager_show_splash();
    sleep_ms(800);

    // --- WiFi init ---
    // cyw43_arch_wifi_connect_timeout_ms() occasionally reports a timeout
    // right as DHCP is still finishing (association can succeed on the
    // hotspot side a few seconds after the call gives up). Retry a few
    // times, and on a reported failure double-check the actual link/IP
    // state before believing it.
    //
    // Everything past this point must keep working with wifi_ok == false —
    // this cabinet has to run a full local game with no Pi/hotspot present
    // at all, not just tolerate a dead MQTT broker.
    bool wifi_ok = false;
    display_manager_show_status("Connecting WiFi...");
    if (cyw43_arch_init()) {
        printf("[WiFi] init FAILED\n");
        display_manager_show_status("WiFi init failed");
        sleep_ms(1000);
    } else {
        cyw43_arch_enable_sta_mode();

        for (int attempt = 1; attempt <= 3 && !wifi_ok; attempt++) {
            printf("[WiFi] Connecting to '%s' (attempt %d/3)...\n", WIFI_SSID, attempt);
            char status[24];
            snprintf(status, sizeof(status), "WiFi try %d/3...", attempt);
            display_manager_show_status(status);

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
            display_manager_show_status("WiFi OK");
        } else {
            printf("[WiFi] Giving up after 3 attempts — running offline\n");
            display_manager_show_status("Offline (no WiFi)");
        }
        sleep_ms(600);
    }

    // --- MQTT --- (best-effort; game must run fine with the Pi absent)
    if (wifi_ok) {
        mqtt_app_init(MQTT_BROKER_IP, MQTT_BROKER_PORT, PICO_ID, TABLE_ID);
        mqtt_app_on_player(on_pi_player);
        mqtt_app_on_sync(on_pi_sync);
        mqtt_app_on_rfid(on_pi_rfid);

        display_manager_show_status("Connecting to Pi...");
        // Bounded wait so a missing/unreachable Pi never delays boot —
        // just enough to show "Connected" if the broker answers quickly.
        uint32_t wait_start = to_ms_since_boot(get_absolute_time());
        while (!mqtt_app_connected() &&
               to_ms_since_boot(get_absolute_time()) - wait_start < 3000) {
            cyw43_arch_poll();
            sleep_ms(50);
        }
        display_manager_show_status(mqtt_app_connected() ? "Pi connected" : "Pi offline (local mode)");
        sleep_ms(600);
    } else {
        display_manager_show_status("Running offline");
        sleep_ms(600);
    }

    // --- Remaining peripherals ---
    rfid_handler_init();    printf("[init] RFID\n");
    i2c_comms_init();       printf("[init] I2C\n");

    display_manager_show_splash();
    sleep_ms(1000);

    GameData  game;
    game_init(&game);
    _active_game = &game;

    BallData  ball       = {0};
    BallData  fresh      = {0};
    GameState prev_state = GAME_WAITING;
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

        // --- I2C (ball tracker feed, shared by both side boards) ---
        i2c_comms_poll(&fresh);
        if (fresh.valid) {
            ball = fresh;
            game_update(&game, &ball);

            // Only the primary board forwards ball telemetry to the Pi —
            // the secondary board still runs the same local game_update()
            // (for its own display/score), it just doesn't publish it.
            if (PICO_ROLE_PRIMARY && game.state == GAME_PLAYING
                && game.field_w > 0 && game.field_h > 0) {
                float x = ((float)game.ball_x / game.field_w) * 1000.0f;
                float y = ((float)game.ball_y / game.field_h) * 500.0f;
                mqtt_publish_ball(TABLE_ID, x, y, game.current_kmh);
            }
        }

        // --- RFID (this board's own reader; always registers to MY_SIDE) ---
        RFIDScanResult rfid;
        rfid_handler_scan(&rfid);
        if (rfid.result == RFID_NEW_TAP) {
            const char *name = game_lookup_player(rfid.uid);
            printf("[RFID] %02X:%02X:%02X:%02X => %s\n",
                   rfid.uid[0], rfid.uid[1],
                   rfid.uid[2], rfid.uid[3],
                   name ? name : "Guest");

            PlayerSlot *mine = (MY_SIDE == 'A') ? game.side_a : game.side_b;
            int next_slot = mine[0].filled ? 2 : 1;

            char uid_hex[9];
            uid_to_hex(rfid.uid, uid_hex);
            mqtt_publish_rfid(TABLE_ID, PICO_ID, MY_SIDE_STR, next_slot, uid_hex);

            game_register_player(&game, MY_SIDE, rfid.uid, name);
        }

        // --- Display every 250ms ---
        if (loop % 3 == 0)
            display_manager_render(&game);

        // --- Heartbeat every 5s ---
        if (now - last_heartbeat > 5000) {
            last_heartbeat = now;
            mqtt_publish_heartbeat(PICO_ID, TABLE_ID, MY_SIDE_STR,
                                   PICO_ROLE_PRIMARY ? "primary" : "secondary",
                                   _device_ip, FIRMWARE,
                                   (now - boot_ms) / 1000);
        }

        // --- Game state to Pi every second — primary board only ---
        if (PICO_ROLE_PRIMARY
            && (game.state == GAME_PLAYING || game.state == GAME_OVER)
            && now - last_state_pub > 1000) {
            last_state_pub = now;

            const char *state_label =
                (game.state == GAME_OVER) ? "GAME_OVER" : "GAME_PLAYING";
            const char *winner_side = "";
            char winner_uid[9] = "";
            if (game.state == GAME_OVER) {
                winner_side = (game.winner == 1) ? "A" : "B";
                const PlayerSlot *w = (game.winner == 1) ? &game.side_a[0] : &game.side_b[0];
                uid_to_hex(w->uid, winner_uid);
            }

            mqtt_publish_state(TABLE_ID, PICO_ID, _session_id, state_label,
                               local_mode_label(&game), game.score_a, game.score_b,
                               game.fastest_kmh, winner_side, winner_uid,
                               game_elapsed_seconds(&game));
        }

        // --- Serial log every 2s during play ---
        if (now - last_log > 2000) {
            last_log = now;
            if (game.state == GAME_PLAYING) {
                char label_a[34], label_b[34];
                game_side_label(&game, 'A', label_a, sizeof(label_a));
                game_side_label(&game, 'B', label_b, sizeof(label_b));
                printf("[GAME] %s %d-%d %s  %.1fkm/h  t=%lus\n",
                       label_a, game.score_a,
                       game.score_b, label_b,
                       game.current_kmh,
                       game_elapsed_seconds(&game));
            }
        }

        sleep_ms(80);
    }
}
