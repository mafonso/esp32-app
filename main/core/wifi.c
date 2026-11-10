#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
static volatile bool s_hold;  // don't auto-reconnect: credentials are being changed

// State of the last wifi_try(), for the web UI to poll.
static SemaphoreHandle_t s_try_lock;
static wifi_try_state_t s_try_state;
static int s_try_attempt;
static char s_try_msg[160];
static atomic_bool s_trying;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        s_ip[0] = '\0';
        if (s_hold) {
            return;
        }
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

static esp_err_t apply_creds(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    // A fixed 32-byte field: a 32-character SSID has no terminator.
    memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static esp_err_t apply_config(void)
{
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass) != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi configured; use the USB console: wifi <ssid> <password>");
        return ESP_ERR_NOT_FOUND;
    }
    return apply_creds(ssid, pass);
}

// Drop the link without the event handler reconnecting with the old config.
static void disconnect_quietly(void)
{
    s_hold = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // let the disconnect event pass
    xEventGroupClearBits(s_events, CONNECTED_BIT);
    s_ip[0] = '\0';
}

static esp_err_t connect_with(const char *ssid, const char *pass)
{
    disconnect_quietly();
    esp_err_t err = ssid ? apply_creds(ssid, pass) : apply_config();
    s_hold = false;
    s_retries = 0;
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    return err;
}

esp_err_t wifi_start(void)
{
    s_events = xEventGroupCreate();
    s_try_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    if (settings_get_name(s_hostname) != ESP_OK) {
        wifi_default_hostname(s_hostname, sizeof(s_hostname));
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
    return connect_with(NULL, NULL);
}

bool wifi_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & CONNECTED_BIT;
}

static void set_try(wifi_try_state_t state, bool new_attempt, const char *fmt, ...)
{
    xSemaphoreTake(s_try_lock, portMAX_DELAY);
    s_try_state = state;
    if (new_attempt) {
        s_try_attempt++;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_try_msg, sizeof(s_try_msg), fmt, ap);
    va_end(ap);
    xSemaphoreGive(s_try_lock);
}

// Caller has set s_trying.
static esp_err_t do_try(const char *ssid, const char *pass, int timeout_ms)
{
    char old_ssid[SETTINGS_SSID_MAX], old_pass[SETTINGS_PASS_MAX];
    bool have_old = settings_get_wifi(old_ssid, old_pass) == ESP_OK;

    esp_err_t err = connect_with(ssid, pass);
    if (err == ESP_OK && wifi_wait_connected(timeout_ms)) {
        err = settings_set_wifi(ssid, pass);
        if (err != ESP_OK) {
            set_try(WIFI_TRY_FAILED, false, "Connected to '%s' but could not save it: %s", ssid,
                    esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "switched to '%s' and saved it", ssid);
        set_try(WIFI_TRY_OK, false, "Connected to '%s' (%s)", ssid, s_ip);
        return ESP_OK;
    }

    if (!have_old) {
        // Nothing to go back to: keep trying the new network, unsaved.
        set_try(WIFI_TRY_FAILED, false, "Could not connect to '%s' within %d s", ssid, timeout_ms / 1000);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG, "'%s' did not connect within %d s, going back to '%s'", ssid, timeout_ms / 1000, old_ssid);
    connect_with(old_ssid, old_pass);
    set_try(WIFI_TRY_FAILED, false,
            "Could not connect to '%s' within %d s (wrong password or out of range?). Back on '%s'.",
            ssid, timeout_ms / 1000, old_ssid);
    return ESP_ERR_TIMEOUT;
}

static bool valid_creds(const char *ssid, const char *pass)
{
    return ssid[0] && strlen(ssid) < SETTINGS_SSID_MAX && strlen(pass) < SETTINGS_PASS_MAX &&
           (pass[0] == '\0' || strlen(pass) >= 8);
}

esp_err_t wifi_try(const char *ssid, const char *pass, int timeout_ms)
{
    if (!valid_creds(ssid, pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&s_trying, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    set_try(WIFI_TRY_TRYING, true, "Connecting to '%s'", ssid);
    esp_err_t err = do_try(ssid, pass, timeout_ms);
    atomic_store(&s_trying, false);
    return err;
}

typedef struct {
    char ssid[SETTINGS_SSID_MAX];
    char pass[SETTINGS_PASS_MAX];
    int timeout_ms;
} try_args_t;

static void try_task(void *arg)
{
    try_args_t *a = arg;
    // Let the HTTP response that started this go out before the link drops.
    vTaskDelay(pdMS_TO_TICKS(1000));
    do_try(a->ssid, a->pass, a->timeout_ms);
    memset(a, 0, sizeof(*a));
    free(a);
    atomic_store(&s_trying, false);
    vTaskDelete(NULL);
}

esp_err_t wifi_try_async(const char *ssid, const char *pass, int timeout_ms, int *attempt)
{
    if (!valid_creds(ssid, pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    try_args_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(a->ssid, ssid, sizeof(a->ssid));
    strlcpy(a->pass, pass, sizeof(a->pass));
    a->timeout_ms = timeout_ms;
    if (atomic_exchange(&s_trying, true)) {
        free(a);
        return ESP_ERR_INVALID_STATE;
    }
    set_try(WIFI_TRY_TRYING, true, "Connecting to '%s'", ssid);
    if (attempt) {
        *attempt = s_try_attempt;  // only changes while s_trying is held
    }
    if (xTaskCreate(try_task, "wifi_try", 4096, a, 5, NULL) != pdPASS) {
        set_try(WIFI_TRY_FAILED, false, "out of memory");
        free(a);
        atomic_store(&s_trying, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_try_status(wifi_try_state_t *state, int *attempt, char *msg, size_t msg_len)
{
    xSemaphoreTake(s_try_lock, portMAX_DELAY);
    *state = s_try_state;
    *attempt = s_try_attempt;
    strlcpy(msg, s_try_msg, msg_len);
    xSemaphoreGive(s_try_lock);
}

const char *wifi_try_state_name(wifi_try_state_t s)
{
    switch (s) {
    case WIFI_TRY_TRYING: return "trying";
    case WIFI_TRY_OK: return "ok";
    case WIFI_TRY_FAILED: return "failed";
    default: return "idle";
    }
}

esp_err_t wifi_scan(wifi_ap_record_t **out, uint16_t *count)
{
    *out = NULL;
    *count = 0;
    if (atomic_load(&s_trying)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) {
        n = 40;
    }
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(*aps));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&n, aps);  // also frees the driver's list
    if (err != ESP_OK) {
        free(aps);
        return err;
    }
    *out = aps;
    *count = n;
    return ESP_OK;
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

void wifi_default_hostname(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%s-%02x%02x%02x", CONFIG_APP_HOSTNAME_PREFIX, mac[3], mac[4], mac[5]);
}
