// main/todo_usb.c
// USB 串口任务通道。行协议:每条消息占一行,以 '#' 开头,行内为 JSON。
//
//   主机 -> 设备:
//     {"cmd":"ping"}
//     {"cmd":"list"}
//     {"cmd":"set","tasks":[{"id":"..","title":"..","notes":"..","status":"todo|done","urgent":false}, ...]}
//
//   设备 -> 主机:
//     {"ok":true,"cmd":"list","count":N,"total":N,"done":M,"serverVersion":V,"tasks":[...]}
//     {"event":"changed", ...同上...}            // 设备侧(按键或 Wi-Fi 同步)改动后主动上报
//     {"ok":true,"cmd":"ping","fw":"1.0.0-usb"}
//     {"ok":false,"error":"bad_json|bad_tasks|unknown_cmd"}
//
// 设计要点:
//  * 读取用 USB-Serial-JTAG 驱动;输出走 printf(与控制台日志共用同一把锁,保证一行不被日志切碎)。
//  * 任务清单以 NVS(namespace todo_tasks, key list)为持久化存储,断电/拔 USB 不丢。
//  * 本模块不改渲染与 Wi-Fi 同步逻辑,只通过 todo_app_export_tasks() 只读导出、
//    以及复用 todo_app_apply_remote_tasks() 写入清单。
//  * 写入时不改变 server_version,避免误触发 Wi-Fi 侧重同步。
#include "todo_usb.h"

#include "todo_app.h"
#include "todo_model.h"
#include "bsp_display.h"

#include "driver/usb_serial_jtag.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG            "todo_usb"
#define USB_LINE_MAX       (6144)
#define NVS_NS         "todo_tasks"
#define NVS_KEY        "list"
#define CHANGE_POLL_MS (1000)
#define FW_TAG         "1.0.0-usb"

static char   s_line[USB_LINE_MAX];
static size_t s_line_len;
static uint32_t s_sig;
static bool   s_ready;

// ---------------------------------------------------------------------------
// 与小工具
// ---------------------------------------------------------------------------
static const char *json_str(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

static void emit_raw(const char *json)
{
    if (!json) return;
    printf("#%s\n", json);
    fflush(stdout);
}

static void emit_error(const char *code)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", code ? code : "error");
    emit_raw(buf);
}

// ---------------------------------------------------------------------------
// 导出当前清单(加锁,调用方无需持锁)
// ---------------------------------------------------------------------------
static int export_tasks(todo_app_remote_task_t *out, int max_count)
{
    if (!bsp_lvgl_lock(500)) return 0;
    int n = todo_app_export_tasks(out, max_count);
    bsp_lvgl_unlock();
    return n;
}

static void read_report(int *total, int *done, int *ver)
{
    if (bsp_lvgl_lock(500)) {
        todo_app_get_report(total, done, ver);
        bsp_lvgl_unlock();
    }
}

// kind/key 二选一:kind="cmd"/"event",value 为对应字符串
static char *list_json(const char *kind, const char *value)
{
    todo_app_remote_task_t items[TODO_APP_MAX_TASKS];
    int n = export_tasks(items, TODO_APP_MAX_TASKS);
    int total = 0, done = 0, ver = 0;
    read_report(&total, &done, &ver);

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject(root, "ok", true);
    if (kind) cJSON_AddStringToObject(root, kind, value ? value : "");
    cJSON_AddNumberToObject(root, "count", n);
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddNumberToObject(root, "done", done);
    cJSON_AddNumberToObject(root, "serverVersion", ver);
    cJSON *arr = cJSON_AddArrayToObject(root, "tasks");
    for (int i = 0; arr && i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "id", items[i].id);
        cJSON_AddStringToObject(o, "title", items[i].title);
        cJSON_AddStringToObject(o, "notes", items[i].notes);
        cJSON_AddStringToObject(o, "status",
                                items[i].state == TODO_STATE_DONE ? "done" : "todo");
        cJSON_AddBoolToObject(o, "urgent", items[i].state == TODO_STATE_URGENT);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

// 清单指纹:用于发现「任何来源」的变化(设备按键 / Wi-Fi 同步 / USB set)
static uint32_t list_sig(void)
{
    todo_app_remote_task_t items[TODO_APP_MAX_TASKS];
    int n = export_tasks(items, TODO_APP_MAX_TASKS);
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        const char *parts[3] = { items[i].id, items[i].title, items[i].notes };
        for (int p = 0; p < 3; p++) {
            for (const char *s = parts[p]; s && *s; s++) {
                h ^= (unsigned char)*s;
                h *= 16777619u;
            }
            h ^= 0x1Fu;
            h *= 16777619u;
        }
        h ^= (uint32_t)items[i].state + 1u;
        h *= 16777619u;
    }
    h ^= (uint32_t)n;
    h *= 16777619u;
    return h;
}

// ---------------------------------------------------------------------------
// NVS 持久化
// ---------------------------------------------------------------------------
static void nvs_save(const char *json)
{
    if (!json) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 写失败");
        return;
    }
    if (nvs_set_str(h, NVS_KEY, json) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "nvs_set_str 失败(清单可能过长)");
    }
    nvs_close(h);
}

static char *nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    char *out = NULL;
    if (nvs_get_str(h, NVS_KEY, NULL, &len) == ESP_OK && len > 1 && len < USB_LINE_MAX) {
        out = (char *)malloc(len);
        if (out && nvs_get_str(h, NVS_KEY, out, &len) != ESP_OK) {
            free(out);
            out = NULL;
        }
    }
    nvs_close(h);
    return out;
}

// ---------------------------------------------------------------------------
// 应用一份清单
// ---------------------------------------------------------------------------
static int apply_from_json(cJSON *root)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (!cJSON_IsArray(arr)) return -1;

    todo_app_remote_task_t items[TODO_APP_MAX_TASKS];
    memset(items, 0, sizeof(items));
    int n = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (n >= TODO_APP_MAX_TASKS) break;
        if (!cJSON_IsObject(it)) continue;
        const char *id = json_str(it, "id");
        const char *title = json_str(it, "title");
        if (!id || !*id || !title || !*title) continue;   // 与 apply_remote 的过滤条件一致
        const char *notes = json_str(it, "notes");
        const char *status = json_str(it, "status");
        bool urgent = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "urgent"));

        todo_state_t st = TODO_STATE_PENDING;
        if (status && strcmp(status, "done") == 0) {
            st = TODO_STATE_DONE;
        } else if (urgent) {
            st = TODO_STATE_URGENT;
        }
        snprintf(items[n].id, sizeof(items[n].id), "%s", id);
        snprintf(items[n].title, sizeof(items[n].title), "%s", title);
        snprintf(items[n].notes, sizeof(items[n].notes), "%s", notes ? notes : "");
        items[n].state = st;
        items[n].urgent = (st == TODO_STATE_URGENT);
        items[n].deleted = false;
        items[n].priority = 0;
        items[n].sort_order = n;
        n++;
    }

    int total = 0, done = 0, ver = 0;
    if (!bsp_lvgl_lock(1000)) return -2;
    todo_app_get_report(&total, &done, &ver);            // 沿用当前版本,不触发 Wi-Fi 重同步
    todo_app_apply_remote_tasks(items, n, ver);
    bsp_lvgl_unlock();
    return n;
}

// ---------------------------------------------------------------------------
// 命令处理
// ---------------------------------------------------------------------------
static void handle_line(char *line)
{
    if (!line || line[0] != '#') return;                 // 忽略日志等无关输出
    cJSON *root = cJSON_Parse(line + 1);
    if (!root) {
        emit_error("bad_json");
        return;
    }

    const char *cmd = json_str(root, "cmd");
    if (cmd && strcmp(cmd, "ping") == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"cmd\":\"ping\",\"fw\":\"%s\"}", FW_TAG);
        emit_raw(buf);
    } else if (cmd && strcmp(cmd, "list") == 0) {
        char *txt = list_json("cmd", "list");
        emit_raw(txt);
        free(txt);
    } else if (cmd && strcmp(cmd, "set") == 0) {
        int n = apply_from_json(root);
        if (n < 0) {
            emit_error("bad_tasks");
        } else {
            char *txt = list_json("cmd", "list");
            if (txt) {
                nvs_save(txt);
                emit_raw(txt);
                free(txt);
            }
            s_sig = list_sig();
        }
    } else {
        emit_error("unknown_cmd");
    }
    cJSON_Delete(root);
}

static void consume_lines(void)
{
    size_t start = 0;
    for (size_t i = 0; i < s_line_len; i++) {
        if (s_line[i] != '\n') continue;
        size_t len = i - start;
        if (len && s_line[start + len - 1] == '\r') len--;
        s_line[start + len] = '\0';
        if (len) handle_line(&s_line[start]);
        start = i + 1;
    }
    if (start > 0) {
        size_t rem = s_line_len - start;
        if (rem) memmove(s_line, s_line + start, rem);
        s_line_len = rem;
        s_line[s_line_len] = '\0';
    } else {
        s_line[s_line_len] = '\0';
    }
}

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------
static void usb_task(void *arg)
{
    (void)arg;

    char *saved = nvs_load();
    if (saved) {
        cJSON *root = cJSON_Parse(saved);
        if (root) {
            int n = apply_from_json(root);
            ESP_LOGI(TAG, "NVS 清单已恢复: %d 条", n);
            cJSON_Delete(root);
        }
        free(saved);
    }
    s_sig = list_sig();                                  // 基线:避免开机误报 changed
    TickType_t last = xTaskGetTickCount();
    ESP_LOGI(TAG, "USB 任务通道就绪(#{\"cmd\":\"list\"})");

    for (;;) {
        if (s_line_len < USB_LINE_MAX - 1) {
            int n = usb_serial_jtag_read_bytes(s_line + s_line_len,
                                                (uint32_t)(USB_LINE_MAX - 1 - s_line_len),
                                                pdMS_TO_TICKS(150));
            if (n > 0) {
                s_line_len += (size_t)n;
                consume_lines();
            }
        } else {
            ESP_LOGW(TAG, "行缓冲溢出,丢弃");
            s_line_len = 0;
        }

        if (xTaskGetTickCount() - last >= pdMS_TO_TICKS(CHANGE_POLL_MS)) {
            last = xTaskGetTickCount();
            uint32_t sig = list_sig();
            if (sig != s_sig) {
                s_sig = sig;
                char *txt = list_json("event", "changed");
                if (txt) {
                    nvs_save(txt);
                    emit_raw(txt);
                    free(txt);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void todo_usb_start(void)
{
    if (s_ready) return;
    s_line_len = 0;
    s_line[0] = '\0';
    s_sig = 0;

    // 必须自己确保 NVS 就绪:本函数在 todo_sync_start() 之前被调用,
    // 而后者(经 todo_config_load)才会 nvs_flash_init(),否则本模块启动时
    // nvs_open 会返回 ESP_ERR_NVS_NOT_INITIALIZED,导致清单读不回来。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init 失败(%s),本次无法持久化任务", esp_err_to_name(nvs_err));
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size = 2048;
        cfg.tx_buffer_size = 512;
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB-Serial-JTAG 驱动安装失败(%s),串口命令通道不可用",
                     esp_err_to_name(err));
            return;
        }
    }
    if (xTaskCreate(usb_task, "todo_usb", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        return;
    }
    s_ready = true;
}
