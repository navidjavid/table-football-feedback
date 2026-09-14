#pragma once
#include "pico_mqtt.h"  // MqttKnownPlayer, MAX_KNOWN_PLAYERS

// Persists the full registered-player directory (uid_hex + name pairs,
// exactly as received over MQTT) to the last flash sector, so a board
// still has real player names to resolve offline after a power cycle —
// not just whatever was in RAM this session.
//
// IMPORTANT: flash is rated for a finite number of erase/program cycles
// (commonly ~100,000). The server re-publishes this same list on EVERY
// heartbeat (~every 5s) regardless of whether it changed, so
// player_store_save() must only actually touch flash when the content
// is different from what's already stored — see its own comment.
//
// IMPORTANT: erasing/programming flash halts code execution from flash
// (XIP) for the duration, so interrupts must be disabled around it —
// this means the I2C slave ISR (ball data + peer-tap) cannot fire during
// that window. The write only happens on a genuine content change
// (rare — an admin adding/renaming a player), and takes on the order of
// tens of milliseconds, but a badly-timed sync during active play could
// cause a brief, one-off gap in ball tracking. This is untested on real
// hardware.

// Loads the persisted directory into `out` (up to max_count entries).
// Returns the number of entries found, or 0 if flash has never been
// written by this module, or contains a corrupt/mismatched record (e.g.
// a factory-fresh board, or firmware built with a different
// MqttKnownPlayer layout).
int player_store_load(MqttKnownPlayer *out, int max_count);

// Persists `players` (count entries, capped at MAX_KNOWN_PLAYERS) to
// flash, replacing whatever was there — but only if it actually differs
// from the currently-stored content. Safe to call on every sync;
// no-ops (and doesn't touch flash at all) when nothing changed.
void player_store_save(const MqttKnownPlayer *players, int count);
