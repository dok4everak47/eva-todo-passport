// main/todo_sync.h
// Wi-Fi + Cloudflare Worker sync for EVA Todo.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void todo_sync_start(void);

// Lightweight status snapshot for the LVGL settings/status indicator.
bool todo_sync_wifi_connected(void);
bool todo_sync_server_connected(void);
const char *todo_sync_ip_address(void);
bool todo_sync_provisioning(void);
const char *todo_sync_ap_ip_address(void);
const char *todo_sync_ap_ssid(void);
const char *todo_sync_api_base_url(void);
uint16_t todo_sync_api_port(void);
bool todo_sync_dhcp_enabled(void);
