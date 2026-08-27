#pragma once
#include "game_logic.h"

void display_manager_init(void);
void display_manager_render(const GameData *g);
void display_manager_show_splash(void);

// Boot-time status line under the splash banner, e.g. "Connecting WiFi...",
// "WiFi OK, connecting Pi...", "Offline mode" — one line, kept short.
void display_manager_show_status(const char *line);
