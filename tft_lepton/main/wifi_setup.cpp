// Optional WiFi station mode, configured from /sdcard/wifi.json
// ({"ssid": "...", "password": "..."}) rather than hardcoded or set via
// menuconfig -- the SD card is already this project's natural place for
// per-deployment config that shouldn't live in source control. Entirely
// optional: if the file is missing, unreadable, or malformed, WiFi is
// simply never started and the rest of the app doesn't care either way.

#include "wifi_setup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_setup";

static char s_ip_str[16] = {0};
static volatile bool s_got_ip = false;
static bool s_started = false;   // guards wifi_setup_stop() when wifi.json was never found/valid

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, retrying...");
        s_got_ip = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_got_ip = true;
    }
}

// Reads and parses json_path for "ssid"/"password" string fields. Returns
// false (leaving ssid/password untouched) on any failure -- missing file,
// oversized file, bad JSON, missing/non-string fields -- all treated the
// same: WiFi just doesn't start.
static bool read_wifi_json(const char *json_path, char *ssid, size_t ssid_len,
                            char *password, size_t password_len)
{
    FILE *f = fopen(json_path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "%s not found, WiFi not configured", json_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4096) {
        ESP_LOGE(TAG, "%s: unreasonable size (%ld bytes)", json_path, size);
        fclose(f);
        return false;
    }

    char *buf = static_cast<char *>(malloc((size_t)size + 1));
    if (!buf) {
        fclose(f);
        return false;
    }
    const size_t read_len = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "%s: JSON parse failed", json_path);
        return false;
    }

    bool ok = false;
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(ssid_item) && ssid_item->valuestring != nullptr &&
        cJSON_IsString(pass_item) && pass_item->valuestring != nullptr) {
        strncpy(ssid, ssid_item->valuestring, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        strncpy(password, pass_item->valuestring, password_len - 1);
        password[password_len - 1] = '\0';
        ok = true;
    } else {
        ESP_LOGE(TAG, "%s: missing/invalid \"ssid\" or \"password\"", json_path);
    }
    cJSON_Delete(root);
    return ok;
}

void wifi_setup_start(const char *json_path)
{
    char ssid[33] = {0};       // wifi_sta_config_t.ssid is 32 bytes
    char password[65] = {0};   // wifi_sta_config_t.password is 64 bytes
    if (!read_wifi_json(json_path, ssid, sizeof(ssid), password, sizeof(password))) {
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          &wifi_event_handler, nullptr, nullptr));

    // Zero-init + assign individually, not a partial designated-init list --
    // same reasoning as elsewhere in this project: wifi_config_t/
    // wifi_sta_config_t have many more fields than we care about, and this
    // project's build treats -Wmissing-field-initializers as an error for
    // partial designated inits. Leaving `sta.threshold` zeroed is
    // intentional, not an oversight: per esp_wifi_types_generic.h, an unset
    // threshold with a password set defaults to WPA2, which is exactly what
    // wifi.json's shape (ssid+password, nothing else) implies.
    wifi_config_t wifi_config = {};
    strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid));
    strncpy(reinterpret_cast<char *>(wifi_config.sta.password), password,
            sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    ESP_LOGI(TAG, "Connecting to SSID \"%s\"...", ssid);
}

bool wifi_setup_get_ip(char *ip_str, size_t ip_str_len)
{
    if (!s_got_ip) {
        return false;
    }
    strncpy(ip_str, s_ip_str, ip_str_len - 1);
    ip_str[ip_str_len - 1] = '\0';
    return true;
}

void wifi_setup_stop(void)
{
    if (!s_started) {
        return;
    }
    esp_wifi_stop();
    s_started = false;
    s_got_ip = false;
    ESP_LOGI(TAG, "WiFi stopped");
}
