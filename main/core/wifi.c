#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "mdns.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static char s_hostname[32];
static char s_ip[16];
static int s_retries;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        s_ip[0] = '\0';
        if (s_retries++ < 5 || s_retries % 20 == 0) {
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", ev->reason);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "connected, ip %s, http://%s.local/", s_ip, s_hostname);
        s_retries = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

static esp_err_t apply_config(void)
{
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass) != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi configured; use the USB console: wifi <ssid> <password>");
        return ESP_ERR_NOT_FOUND;
    }
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

esp_err_t wifi_start(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    if (settings_get_name(s_hostname) != ESP_OK) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_hostname, sizeof(s_hostname), "%s-%02x%02x%02x",
                 CONFIG_APP_HOSTNAME_PREFIX, mac[3], mac[4], mac[5]);
    }
    esp_netif_set_hostname(s_netif, s_hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    // Lower latency for the web UI; the board is USB powered.
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set("ESP32-S3 app");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    return ESP_OK;
}

esp_err_t wifi_reconnect(void)
{
    esp_wifi_disconnect();
    esp_err_t err = apply_config();
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    return err;
}

bool wifi_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & CONNECTED_BIT;
}

void wifi_get_status(wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = xEventGroupGetBits(s_events) & CONNECTED_BIT;
    wifi_ap_record_t ap;
    if (out->connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(out->ssid, (char *)ap.ssid, sizeof(out->ssid));
        out->rssi = ap.rssi;
    }
    strlcpy(out->ip, s_ip, sizeof(out->ip));
}

const char *wifi_hostname(void)
{
    return s_hostname;
}
