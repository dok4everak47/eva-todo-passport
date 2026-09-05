// main/todo_config.h
// Public defaults + optional private overrides.
//
// Put real credentials in main/firmware_private.h (ignored by Git). The tracked
// defaults below keep the project buildable without secrets.
#pragma once

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
