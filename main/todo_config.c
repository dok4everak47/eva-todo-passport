#include "todo_config.h"

#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static void copy_value(char *dst, size_t size, const char *src)
{
    if (size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

static void set_defaults(todo_runtime_config_t *c)
{
    memset(c, 0, sizeof(*c));
    copy_value(c->wifi_ssid, sizeof(c->wifi_ssid), TODO_WIFI_SSID);
    copy_value(c->wifi_password, sizeof(c->wifi_password), TODO_WIFI_PASSWORD);
    copy_value(c->api_base_url, sizeof(c->api_base_url), TODO_API_BASE_URL);
    copy_value(c->api_token, sizeof(c->api_token), TODO_API_TOKEN);
    c->dhcp = true;
    copy_value(c->static_ip, sizeof(c->static_ip), "192.168.10.117");
    copy_value(c->gateway, sizeof(c->gateway), "192.168.10.1");
    copy_value(c->netmask, sizeof(c->netmask), "255.255.255.0");
    c->api_port = 443;
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t required = size;
    if (nvs_get_str(h, key, NULL, &required) == ESP_OK && required > 0 && required <= size) {
        (void)nvs_get_str(h, key, dst, &required);
    }
}

void todo_config_load(todo_runtime_config_t *config)
{
    if (!config) return;
    set_defaults(config);
    esp_err_t flash_err = nvs_flash_init();
    if (flash_err == ESP_ERR_NVS_NO_FREE_PAGES || flash_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        flash_err = nvs_flash_init();
    }
    if (flash_err != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open("todo_cfg", NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    get_str(h, "password", config->wifi_password, sizeof(config->wifi_password));
    get_str(h, "api_url", config->api_base_url, sizeof(config->api_base_url));
    get_str(h, "api_token", config->api_token, sizeof(config->api_token));
    get_str(h, "static_ip", config->static_ip, sizeof(config->static_ip));
    get_str(h, "gateway", config->gateway, sizeof(config->gateway));
    get_str(h, "netmask", config->netmask, sizeof(config->netmask));
    uint8_t dhcp = config->dhcp ? 1 : 0;
    uint16_t port = config->api_port;
    (void)nvs_get_u8(h, "dhcp", &dhcp);
    (void)nvs_get_u16(h, "api_port", &port);
    config->dhcp = dhcp != 0;
    config->api_port = port ? port : 443;
    nvs_close(h);
}

bool todo_config_save(const todo_runtime_config_t *config)
{
    if (!config || !config->wifi_ssid[0] || !config->api_base_url[0]) return false;
    if (nvs_flash_init() != ESP_OK) return false;
    nvs_handle_t h;
    if (nvs_open("todo_cfg", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", config->wifi_ssid) == ESP_OK &&
              nvs_set_str(h, "password", config->wifi_password) == ESP_OK &&
              nvs_set_str(h, "api_url", config->api_base_url) == ESP_OK &&
              nvs_set_str(h, "api_token", config->api_token) == ESP_OK &&
              nvs_set_str(h, "static_ip", config->static_ip) == ESP_OK &&
              nvs_set_str(h, "gateway", config->gateway) == ESP_OK &&
              nvs_set_str(h, "netmask", config->netmask) == ESP_OK &&
              nvs_set_u8(h, "dhcp", config->dhcp ? 1 : 0) == ESP_OK &&
              nvs_set_u16(h, "api_port", config->api_port ? config->api_port : 443) == ESP_OK;
    if (ok) ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
