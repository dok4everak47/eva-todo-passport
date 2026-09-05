// main/todo_sync.h
// Wi-Fi + Cloudflare Worker sync for EVA Todo.
#pragma once

#include <stdbool.h>

void todo_sync_start(void);

// Lightweight status snapshot for the LVGL settings/status indicator.
bool todo_sync_wifi_connected(void);
bool todo_sync_server_connected(void);
const char *todo_sync_ip_address(void);
