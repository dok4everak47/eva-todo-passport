// main/todo_config.h
// Public defaults + optional private overrides.
//
// Put real credentials in main/firmware_private.h (ignored by Git). The tracked
// defaults below keep the project buildable without secrets.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if __has_include("firmware_private.h")
#include "firmware_private.h"
#endif

#ifndef TODO_WIFI_SSID
#define TODO_WIFI_SSID "CHANGE_ME"
#endif

#ifndef TODO_WIFI_PASSWORD
#define TODO_WIFI_PASSWORD "CHANGE_ME"
#endif

#ifndef TODO_API_BASE_URL
#define TODO_API_BASE_URL "https://eva-todo-api.example.workers.dev/api/v1"
#endif

#ifndef TODO_API_TOKEN
#define TODO_API_TOKEN "CHANGE_ME"
#endif

#ifndef TODO_DEVICE_ID
#define TODO_DEVICE_ID "ai-passport-001"
#endif

#ifndef TODO_SYNC_INTERVAL_MS
#define TODO_SYNC_INTERVAL_MS 15000
#endif

#ifndef TODO_REPORT_EVERY_N_SYNCS
#define TODO_REPORT_EVERY_N_SYNCS 4
#endif

#define TODO_CFG_SSID_LEN 33
#define TODO_CFG_PASSWORD_LEN 65
#define TODO_CFG_URL_LEN 160
#define TODO_CFG_TOKEN_LEN 128
#define TODO_CFG_IP_LEN 16

typedef struct {
    char wifi_ssid[TODO_CFG_SSID_LEN];
    char wifi_password[TODO_CFG_PASSWORD_LEN];
    char api_base_url[TODO_CFG_URL_LEN];
    char api_token[TODO_CFG_TOKEN_LEN];
    bool dhcp;
    char static_ip[TODO_CFG_IP_LEN];
    char gateway[TODO_CFG_IP_LEN];
    char netmask[TODO_CFG_IP_LEN];
    uint16_t api_port;
} todo_runtime_config_t;

void todo_config_load(todo_runtime_config_t *config);
bool todo_config_save(const todo_runtime_config_t *config);
