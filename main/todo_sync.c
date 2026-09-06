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
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
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
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "lwip/inet.h"

static const char *TAG = "todo_sync";

#define WIFI_CONNECTED_BIT BIT0
#define MAX_HTTP_BODY 8192
#define MAX_MUTATIONS 8
#define AP_IP_ADDRESS "192.168.192.1"

typedef struct {
    char id[TODO_APP_ID_LEN];
    todo_state_t state;
    bool deleted;
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
static bool s_server_connected;
static char s_ip_address[16] = "0.0.0.0";
static char s_ap_ip_address[16] = AP_IP_ADDRESS;
static char s_ap_ssid[33] = "EVA-PASSPORT";
static bool s_provisioning;
static httpd_handle_t s_httpd;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static todo_runtime_config_t s_config;
// The ESP-IDF HTTP task has a small stack; keep the HTML response off-stack.
static char s_config_page[4096];

static void queue_mutation(const char *id, todo_state_t state, void *user);
static void queue_delete(const char *id, void *user);
static void sync_task(void *arg);
static void start_config_server(void);
static void start_provisioning(void);

static const char CONFIG_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
    "<title>EVA PASSPORT CONFIG</title><style>body{background:#000;color:#ffdc00;font:16px Arial;max-width:560px;margin:20px auto;padding:12px}"
    "h1{background:#95ef5e;color:#000;padding:12px;font-size:22px}label{display:block;margin:12px 0 4px}"
    "input{width:100%%;box-sizing:border-box;background:#050505;color:#ffdc00;border:1px solid #ffdc00;padding:10px}"
    "button{margin-top:18px;background:#95ef5e;border:0;padding:12px 18px;font-weight:bold}"
    "fieldset{border:1px solid #ffdc00;margin:14px 0;padding:10px}legend{color:#95ef5e}"
    "small{color:#aaa}.ok{color:#95ef5e}</style></head>"
    "<body><h1>EVA PASSPORT / DEVICE CONFIG</h1>"
    "<p class='ok'>当前地址: http://%s/</p>"
    "<p>首次配置:连接设备热点后打开本页。保存后设备会重启并连接新的 Wi-Fi。</p>"
    "<fieldset><legend>WIFI / NETWORK</legend>"
    "<form method='post' action='/save'><label>WIFI SSID</label><input name='ssid' maxlength='32' value='%s'>"
    "<label>WIFI PASSWORD</label><input name='password' type='password' maxlength='64' value='%s'>"
    "<label><input name='dhcp' type='checkbox' %s> DHCP</label>"
    "<label>STATIC IP</label><input name='ip' value='%s'><label>GATEWAY</label><input name='gw' value='%s'>"
    "<label>NETMASK</label><input name='mask' value='%s'></fieldset>"
    "<fieldset><legend>CLOUD API</legend><label>SERVER URL</label>"
    "<input name='url' value='%s' placeholder='https://your-server.example/api/v1'>"
    "<label>SERVER PORT</label><input name='port' type='number' value='%u'>"
    "<label>DEVICE TOKEN</label><input name='token' type='password' value='%s'></fieldset>"
    "<button type='submit'>SAVE AND RESTART</button></form>"
    "<p><small>DHCP 推荐用于普通家庭/办公室网络；使用静态 IP 时填写 IP、网关和掩码。"
    "设备配置网页始终使用 HTTP :80，云端端口单独填写。</small></p></body></html>";

static bool configured(void)
{
    return s_config.wifi_ssid[0] && s_config.api_base_url[0] &&
           strcmp(s_config.wifi_ssid, "CHANGE_ME") != 0 &&
           strcmp(s_config.api_base_url, "https://eva-todo-api.example.workers.dev/api/v1") != 0 &&
           strcmp(s_config.api_token, "CHANGE_ME") != 0;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_provisioning) esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        snprintf(s_ip_address, sizeof(s_ip_address), "0.0.0.0");
        esp_wifi_connect();
        ESP_LOGW(TAG, "wifi disconnected, reconnecting");
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "wifi connected, ip=%s", s_ip_address);
        start_config_server();
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
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wifi_config = { 0 };
    size_t ssid_len = strlen(s_config.wifi_ssid);
    if (ssid_len >= sizeof(wifi_config.sta.ssid)) ssid_len = sizeof(wifi_config.sta.ssid) - 1;
    memcpy(wifi_config.sta.ssid, s_config.wifi_ssid, ssid_len);
    wifi_config.sta.ssid[ssid_len] = '\0';
    size_t password_len = strlen(s_config.wifi_password);
    if (password_len >= sizeof(wifi_config.sta.password)) password_len = sizeof(wifi_config.sta.password) - 1;
    memcpy(wifi_config.sta.password, s_config.wifi_password, password_len);
    wifi_config.sta.password[password_len] = '\0';
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (!s_config.dhcp) {
        esp_netif_ip_info_t info = { 0 };
        info.ip.addr = ipaddr_addr(s_config.static_ip);
        info.gw.addr = ipaddr_addr(s_config.gateway);
        info.netmask.addr = ipaddr_addr(s_config.netmask);
        (void)esp_netif_dhcpc_stop(s_sta_netif);
        (void)esp_netif_set_ip_info(s_sta_netif, &info);
    }
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

static void build_api_url(char *out, size_t out_size, const char *path)
{
    const char *base = s_config.api_base_url[0] ? s_config.api_base_url : TODO_API_BASE_URL;
    int port = s_config.api_port ? s_config.api_port : 443;
    bool https = strncmp(base, "https://", 8) == 0;
    bool default_port = (https && port == 443) || (!https && port == 80);
    if (default_port || strstr(base, "://") == NULL) {
        snprintf(out, out_size, "%s%s", base, path);
        return;
    }
    const char *authority = strstr(base, "://") + 3;
    const char *slash = strchr(authority, '/');
    const char *colon = strchr(authority, ':');
    if (colon && (!slash || colon < slash)) {
        snprintf(out, out_size, "%s%s", base, path);
        return;
    }
    if (slash) {
        int prefix = (int)(slash - base);
        snprintf(out, out_size, "%.*s:%d%s%s", prefix, base, port, slash, path);
    } else {
        snprintf(out, out_size, "%s:%d%s", base, port, path);
    }
}

static void form_value(const char *body, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    char needle[48];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return;
    p += strlen(needle);
    size_t n = 0;
    while (p[n] && p[n] != '&' && n + 1 < out_size) n++;
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < out_size; i++) {
        if (p[i] == '+' && w + 1 < out_size) out[w++] = ' ';
        else if (p[i] == '%' && i + 2 < n) {
            char hex[3] = { p[i + 1], p[i + 2], 0 };
            out[w++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else out[w++] = p[i];
    }
    out[w] = '\0';
}

static esp_err_t config_page_handler(httpd_req_t *req)
{
    const char *ip = s_provisioning ? s_ap_ip_address : s_ip_address;
    int n = snprintf(s_config_page, sizeof(s_config_page), CONFIG_HTML,
                     ip,
                     s_config.wifi_ssid, s_config.wifi_password,
                     s_config.dhcp ? "checked" : "",
                     s_config.static_ip, s_config.gateway, s_config.netmask,
                     s_config.api_base_url, (unsigned)s_config.api_port,
                     s_config.api_token);
    if (n < 0) return ESP_FAIL;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_config_page,
                           n < (int)sizeof(s_config_page) ? n : HTTPD_RESP_USE_STRLEN);
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t config_save_handler(httpd_req_t *req)
{
    char body[2048];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) return ESP_FAIL;
    body[received] = '\0';

    todo_runtime_config_t next = s_config;
    form_value(body, "ssid", next.wifi_ssid, sizeof(next.wifi_ssid));
    form_value(body, "password", next.wifi_password, sizeof(next.wifi_password));
    form_value(body, "url", next.api_base_url, sizeof(next.api_base_url));
    form_value(body, "token", next.api_token, sizeof(next.api_token));
    form_value(body, "ip", next.static_ip, sizeof(next.static_ip));
    form_value(body, "gw", next.gateway, sizeof(next.gateway));
    form_value(body, "mask", next.netmask, sizeof(next.netmask));
    char port[8] = { 0 };
    form_value(body, "port", port, sizeof(port));
    int parsed_port = atoi(port);
    if (parsed_port >= 1 && parsed_port <= 65535) next.api_port = (uint16_t)parsed_port;
    next.dhcp = strstr(body, "dhcp=on") != NULL;

    if (!next.wifi_ssid[0] || !next.api_base_url[0] || !next.api_token[0] ||
        !todo_config_save(&next)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid configuration");
    }
    s_config = next;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(req, "Saved. Device is restarting...");
    xTaskCreate(restart_task, "cfg_restart", 2048, NULL, 4, NULL);
    return result;
}

static void start_config_server(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "device config web server failed to start");
        return;
    }
    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = config_page_handler };
    static const httpd_uri_t captive_uri = { .uri = "/generate_204", .method = HTTP_GET, .handler = config_page_handler };
    static const httpd_uri_t hotspot_uri = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = config_page_handler };
    static const httpd_uri_t connect_uri = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = config_page_handler };
    static const httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = config_save_handler };
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &captive_uri);
    httpd_register_uri_handler(s_httpd, &hotspot_uri);
    httpd_register_uri_handler(s_httpd, &connect_uri);
    httpd_register_uri_handler(s_httpd, &save_uri);
    ESP_LOGI(TAG, "device config web server ready on port 80");
}

static void start_provisioning(void)
{
    if (s_provisioning) return;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "EVA-PASSPORT-%02X%02X", mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", (char *)ap.ap.ssid);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_netif_ip_info_t info = { 0 };
    IP4_ADDR(&info.ip, 192, 168, 192, 1);
    IP4_ADDR(&info.gw, 192, 168, 192, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    (void)esp_netif_dhcps_stop(s_ap_netif);
    (void)esp_netif_set_ip_info(s_ap_netif, &info);
    (void)esp_netif_dhcps_start(s_ap_netif);
    s_provisioning = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGW(TAG, "provisioning AP: %s, open network, browse http://%s/",
             ap.ap.ssid, s_ap_ip_address);
    start_config_server();
}

static bool request_json(esp_http_client_method_t method, const char *path,
                         const char *body, http_response_t *resp)
{
    char url[256];
    char auth[160];
    build_api_url(url, sizeof(url), path);
    snprintf(auth, sizeof(auth), "Bearer %s", s_config.api_token);
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
        s_server_connected = false;
        ESP_LOGW(TAG, "HTTP %d %s failed: err=%s status=%d body=%s",
                 (int)method, path, esp_err_to_name(err), resp->status, resp->data);
        return false;
    }
    s_server_connected = true;
    return true;
}

bool todo_sync_wifi_connected(void)
{
    return s_wifi_events &&
           (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool todo_sync_server_connected(void) { return s_server_connected; }
const char *todo_sync_ip_address(void) { return s_ip_address; }
bool todo_sync_provisioning(void) { return s_provisioning; }
const char *todo_sync_ap_ip_address(void) { return s_ap_ip_address; }
const char *todo_sync_ap_ssid(void) { return s_ap_ssid; }
const char *todo_sync_api_base_url(void) { return s_config.api_base_url; }
uint16_t todo_sync_api_port(void) { return s_config.api_port; }
bool todo_sync_dhcp_enabled(void) { return s_config.dhcp; }

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
    s_mutations[slot].deleted = false;
    s_mutations[slot].seq = ++s_mutation_seq;
    s_mutations[slot].pending = true;
    if (s_mutation_lock) xSemaphoreGive(s_mutation_lock);
    ESP_LOGI(TAG, "queued mutation: %s -> %s", id, operation_for_state(state));
}

static void queue_delete(const char *id, void *user)
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
    if (slot < 0) slot = 0;
    snprintf(s_mutations[slot].id, sizeof(s_mutations[slot].id), "%s", id);
    s_mutations[slot].state = TODO_STATE_PENDING;
    s_mutations[slot].deleted = true;
    s_mutations[slot].seq = ++s_mutation_seq;
    s_mutations[slot].pending = true;
    if (s_mutation_lock) xSemaphoreGive(s_mutation_lock);
    ESP_LOGI(TAG, "queued mutation: %s -> delete", id);
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
            muts[i].deleted ? "delete" : operation_for_state(muts[i].state),
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
            pdMS_TO_TICKS(first_sync ? 12000 : TODO_SYNC_INTERVAL_MS));
        if (first_sync && !(bits & WIFI_CONNECTED_BIT)) {
            start_provisioning();
            first_sync = false;
            continue;
        }
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
    todo_config_load(&s_config);
    s_mutation_lock = xSemaphoreCreateMutex();
    todo_app_set_mutation_callback(queue_mutation, NULL);
    todo_app_set_delete_callback(queue_delete, NULL);
    if (!configured()) ESP_LOGW(TAG, "wifi/cloud defaults incomplete; provisioning AP will be available");

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
    // No usable credentials means there is no reason to wait for a STA timeout.
    // Start the local setup network immediately so first boot is discoverable.
    if (!configured()) start_provisioning();
    if (xTaskCreate(sync_task, "todo_sync", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "sync task create failed");
    }
}
