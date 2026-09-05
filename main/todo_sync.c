// main/todo_sync.c
// Offline-first sync layer:
//   * connects to Wi-Fi as STA
//   * periodically POSTs /sync with queued local mutations
//   * applies remote tasks to the LVGL UI under bsp_lvgl_lock()
//   * reports device status through /report
#include "todo_sync.h"
#include "todo_app.h"
#include "todo_config.h"
#include "bsp_display.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "todo_sync";

#define WIFI_CONNECTED_BIT BIT0
#define MAX_HTTP_BODY 8192
#define MAX_MUTATIONS 8

typedef struct {
    char id[TODO_APP_ID_LEN];
    todo_state_t state;
    uint32_t seq;
    bool pending;
} mutation_t;

typedef struct {
    char data[MAX_HTTP_BODY];
    int len;
    int status;
} http_response_t;

static EventGroupHandle_t s_wifi_events;
static mutation_t s_mutations[MAX_MUTATIONS];
static SemaphoreHandle_t s_mutation_lock;
static http_response_t s_http_resp;
static char s_http_body[2048];
static todo_app_remote_task_t s_remote_tasks[TODO_APP_MAX_TASKS];
static uint32_t s_mutation_seq;
static int s_server_version;
static int s_sync_count;
static bool s_started;

static void queue_mutation(const char *id, todo_state_t state, void *user);
static void sync_task(void *arg);

static bool configured(void)
{
    return strcmp(TODO_WIFI_SSID, "CHANGE_ME") != 0 &&
           strcmp(TODO_WIFI_PASSWORD, "CHANGE_ME") != 0 &&
           strcmp(TODO_API_BASE_URL, "https://eva-todo-api.example.workers.dev/api/v1") != 0 &&
           strcmp(TODO_API_TOKEN, "CHANGE_ME") != 0;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGW(TAG, "wifi disconnected, reconnecting");
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "wifi connected");
    }
}

static esp_err_t init_wifi(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wifi_config = { 0 };
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", TODO_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", TODO_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp && evt->data && evt->data_len > 0) {
        int room = (int)sizeof(resp->data) - 1 - resp->len;
        int take = evt->data_len < room ? evt->data_len : room;
        if (take > 0) {
            memcpy(resp->data + resp->len, evt->data, (size_t)take);
            resp->len += take;
            resp->data[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool request_json(esp_http_client_method_t method, const char *path,
                         const char *body, http_response_t *resp)
{
    char url[256];
    char auth[160];
    snprintf(url, sizeof(url), "%s%s", TODO_API_BASE_URL, path);
    snprintf(auth, sizeof(auth), "Bearer %s", TODO_API_TOKEN);
    memset(resp, 0, sizeof(*resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = http_event,
        .user_data = resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Authorization", auth);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }
    esp_err_t err = esp_http_client_perform(client);
    resp->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || resp->status < 200 || resp->status >= 300) {
        ESP_LOGW(TAG, "HTTP %d %s failed: err=%s status=%d body=%s",
                 (int)method, path, esp_err_to_name(err), resp->status, resp->data);
        return false;
    }
    return true;
}

static bool post_json(const char *path, const char *body, http_response_t *resp)
{
    return request_json(HTTP_METHOD_POST, path, body, resp);
}

static bool get_json(const char *path, http_response_t *resp)
{
    return request_json(HTTP_METHOD_GET, path, NULL, resp);
}

static const char *operation_for_state(todo_state_t state)
{
    return state == TODO_STATE_DONE ? "complete" : "reopen";
}

static int json_int(cJSON *obj, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *json_string(cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static int snapshot_mutations(mutation_t out[MAX_MUTATIONS])
{
    int n = 0;
    if (s_mutation_lock) xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_MUTATIONS; i++) {
        if (!s_mutations[i].pending) continue;
        out[n++] = s_mutations[i];
    }
    if (s_mutation_lock) xSemaphoreGive(s_mutation_lock);
    return n;
}

static bool has_pending_mutations(void)
{
    bool pending = false;
    if (s_mutation_lock) xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_MUTATIONS; i++) {
        if (s_mutations[i].pending) {
            pending = true;
            break;
        }
    }
    if (s_mutation_lock) xSemaphoreGive(s_mutation_lock);
    return pending;
}

static void clear_accepted(const mutation_t sent[MAX_MUTATIONS], int count)
{
    if (s_mutation_lock) xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    for (int s = 0; s < count; s++) {
        for (int i = 0; i < MAX_MUTATIONS; i++) {
            if (s_mutations[i].pending &&
                s_mutations[i].seq == sent[s].seq &&
                strcmp(s_mutations[i].id, sent[s].id) == 0) {
                s_mutations[i].pending = false;
            }
        }
    }
    if (s_mutation_lock) xSemaphoreGive(s_mutation_lock);
}

static void queue_mutation(const char *id, todo_state_t state, void *user)
{
    (void)user;
    if (!id || !id[0]) return;

    if (s_mutation_lock) xSemaphoreTake(s_mutation_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_MUTATIONS; i++) {
        if (s_mutations[i].pending && strcmp(s_mutations[i].id, id) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_mutations[i].pending) slot = i;
    }
    if (slot < 0) slot = 0;  // tiny FIFO overwrite: newest user intent wins on badge
    snprintf(s_mutations[slot].id, sizeof(s_mutations[slot].id), "%s", id);
    s_mutations[slot].state = state;
    s_mutations[slot].seq = ++s_mutation_seq;
    s_mutations[slot].pending = true;
    if (s_mutation_lock) xSemaphoreGive(s_mutation_lock);
    ESP_LOGI(TAG, "queued mutation: %s -> %s", id, operation_for_state(state));
}

static void build_sync_body(char *buf, size_t size, const mutation_t muts[MAX_MUTATIONS], int count)
{
    int n = snprintf(buf, size,
        "{\"deviceId\":\"%s\",\"sinceVersion\":%d,\"mutations\":[",
        TODO_DEVICE_ID, s_server_version);
    if (n < 0) n = 0;
    for (int i = 0; i < count && n < (int)size; i++) {
        n += snprintf(buf + n, size - (size_t)n,
            "%s{\"clientMutationId\":\"%lu\",\"operation\":\"%s\",\"task\":{\"id\":\"%s\"}}",
            i ? "," : "",
            (unsigned long)muts[i].seq,
            operation_for_state(muts[i].state),
            muts[i].id);
    }
    if (n < (int)size) snprintf(buf + n, size - (size_t)n, "]}");
    else buf[size - 1] = '\0';
}

static todo_state_t state_from_json(cJSON *task)
{
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(task, "status"));
    cJSON *urgent = cJSON_GetObjectItemCaseSensitive(task, "urgent");
    cJSON *priority = cJSON_GetObjectItemCaseSensitive(task, "priority");
    if (status && strcmp(status, "done") == 0) return TODO_STATE_DONE;
    if ((cJSON_IsTrue(urgent)) || (cJSON_IsNumber(priority) && priority->valueint >= 3)) {
        return TODO_STATE_URGENT;
    }
    return TODO_STATE_PENDING;
}

static void apply_sync_response(const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    if (!root) {
        ESP_LOGW(TAG, "bad sync json");
        return;
    }

    int version = json_int(root, "version", s_server_version);
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    memset(s_remote_tasks, 0, sizeof(s_remote_tasks));
    int count = 0;
    if (cJSON_IsArray(tasks)) {
        cJSON *task = NULL;
        cJSON_ArrayForEach(task, tasks) {
            if (count >= TODO_APP_MAX_TASKS) break;
            const char *id = json_string(task, "id");
            const char *title = json_string(task, "title");
            const char *notes = json_string(task, "notes");
            const char *status = json_string(task, "status");
            const char *deleted = json_string(task, "deletedAt");
            if (!id || !title) continue;
            snprintf(s_remote_tasks[count].id, sizeof(s_remote_tasks[count].id), "%s", id);
            snprintf(s_remote_tasks[count].title, sizeof(s_remote_tasks[count].title), "%s", title);
            snprintf(s_remote_tasks[count].notes, sizeof(s_remote_tasks[count].notes), "%s", notes ? notes : "");
            s_remote_tasks[count].state = state_from_json(task);
            s_remote_tasks[count].urgent = s_remote_tasks[count].state == TODO_STATE_URGENT;
            s_remote_tasks[count].deleted = (deleted && deleted[0]) || (status && strcmp(status, "archived") == 0);
            s_remote_tasks[count].priority = json_int(task, "priority", 1);
            s_remote_tasks[count].sort_order = json_int(task, "sortOrder", count * 10);
            s_remote_tasks[count].version = json_int(task, "version", version);
            count++;
        }
    }

    s_server_version = version;
    if (bsp_lvgl_lock(1000)) {
        todo_app_apply_remote_tasks(s_remote_tasks, count, s_server_version);
        bsp_lvgl_unlock();
    }
    cJSON_Delete(root);
}

static void report_status(void)
{
    int total = 0, done = 0, version = 0;
    int rssi = 0;
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    if (bsp_lvgl_lock(500)) {
        todo_app_get_report(&total, &done, &version);
        bsp_lvgl_unlock();
    }

    snprintf(s_http_body, sizeof(s_http_body),
        "{\"deviceId\":\"%s\",\"firmwareVersion\":\"1.0.0\",\"localVersion\":%d,"
        "\"wifiRssi\":%d,\"taskTotal\":%d,\"taskDone\":%d}",
        TODO_DEVICE_ID, version, rssi, total, done);
    post_json("/report", s_http_body, &s_http_resp);
}

static void sync_once(void)
{
    mutation_t muts[MAX_MUTATIONS] = { 0 };
    int count = snapshot_mutations(muts);
    build_sync_body(s_http_body, sizeof(s_http_body), muts, count);

    if (!post_json("/sync", s_http_body, &s_http_resp)) return;
    clear_accepted(muts, count);
    apply_sync_response(s_http_resp.data);
    s_sync_count++;
    if (s_sync_count == 1 || s_sync_count % TODO_REPORT_EVERY_N_SYNCS == 0) {
        report_status();
    }
}

static bool remote_changed(void)
{
    char path[96];
    snprintf(path, sizeof(path), "/events?sinceVersion=%d&timeoutMs=5000", s_server_version);
    if (!get_json(path, &s_http_resp)) return false;

    cJSON *root = cJSON_Parse(s_http_resp.data);
    if (!root) return false;
    bool changed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "changed"));
    int version = json_int(root, "version", s_server_version);
    cJSON_Delete(root);
    return changed || version > s_server_version;
}

static void sync_task(void *arg)
{
    (void)arg;
    bool first_sync = true;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(first_sync ? 1000 : TODO_SYNC_INTERVAL_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            if (first_sync || has_pending_mutations() || remote_changed()) {
                sync_once();
            }
            first_sync = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void todo_sync_start(void)
{
    if (s_started) return;
    s_started = true;
    s_mutation_lock = xSemaphoreCreateMutex();
    todo_app_set_mutation_callback(queue_mutation, NULL);
    if (!configured()) {
        ESP_LOGW(TAG, "sync disabled: fill main/firmware_private.h first");
        return;
    }

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        ESP_LOGE(TAG, "wifi event group allocation failed");
        return;
    }
    esp_err_t err = init_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }
    if (xTaskCreate(sync_task, "todo_sync", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "sync task create failed");
    }
}
