#include "player_store.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// Reserve the very last flash sector for this block — well clear of the
// program image, which never gets remotely close to filling
// PICO_FLASH_SIZE_BYTES minus one 4KB sector on this board.
#define STORE_MAGIC 0x504C5931u  // ASCII "PLY1", bumped if the layout ever changes
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t        magic;
    uint32_t        count;
    MqttKnownPlayer players[MAX_KNOWN_PLAYERS];
} StoreBlock; // must fit within one FLASH_SECTOR_SIZE (4096B) — it does,
              // with room to spare (8 + 64*40 = 2568 bytes).

static const StoreBlock* _flash_block(void) {
    return (const StoreBlock *)(XIP_BASE + FLASH_TARGET_OFFSET);
}

int player_store_load(MqttKnownPlayer *out, int max_count) {
    const StoreBlock *stored = _flash_block();
    if (stored->magic != STORE_MAGIC) return 0;             // never written, or corrupt
    if (stored->count == 0 || stored->count > MAX_KNOWN_PLAYERS) return 0;

    int n = (int)stored->count;
    if (n > max_count) n = max_count;
    memcpy(out, stored->players, (size_t)n * sizeof(MqttKnownPlayer));
    return n;
}

static bool _matches_stored(const MqttKnownPlayer *players, int count) {
    const StoreBlock *stored = _flash_block();
    if (stored->magic != STORE_MAGIC) return false;
    if (stored->count != (uint32_t)count) return false;
    return memcmp(stored->players, players, (size_t)count * sizeof(MqttKnownPlayer)) == 0;
}

void player_store_save(const MqttKnownPlayer *players, int count) {
    if (count < 0) count = 0;
    if (count > MAX_KNOWN_PLAYERS) count = MAX_KNOWN_PLAYERS;

    // The server re-publishes this same directory on every ~5s
    // heartbeat regardless of whether it changed. Flash is rated for a
    // finite number of erase/program cycles, so skip the write entirely
    // — including the interrupt-disabled window below — unless the
    // content genuinely differs from what's already stored.
    if (_matches_stored(players, count)) return;

    static uint8_t page_buf[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
    memset(page_buf, 0xFF, sizeof(page_buf));
    StoreBlock *block = (StoreBlock *)page_buf;
    block->magic = STORE_MAGIC;
    block->count = (uint32_t)count;
    memcpy(block->players, players, (size_t)count * sizeof(MqttKnownPlayer));

    printf("[STORE] Player directory changed (%d entr%s) - writing to flash...\n",
           count, count == 1 ? "y" : "ies");

    // Erasing/programming flash halts XIP for the duration, so anything
    // that would otherwise fetch code from flash (every ISR here,
    // notably the I2C slave handler) must not fire mid-operation.
    // Interrupts are back on within tens of milliseconds either way.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, page_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    printf("[STORE] Flash write complete.\n");
}
