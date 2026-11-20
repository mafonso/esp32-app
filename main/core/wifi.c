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
#include "esp_timer.h"
#include "esp_log.h"
#include "mdns.h"
#include "settings.h"
#include "wifi.h"
#include "captive_dns.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static char s_hostname[32];
static char s_ip[16];
static int s_retries;
static volatile bool s_hold;  // don't auto-reconnect: credentials are being changed
static volatile bool s_sta_configured;  // a network to join is set in the driver
static atomic_bool s_scanning;          // a scan owns the radio: don't reconnect meanwhile
static esp_timer_handle_t s_retry_timer;
static volatile int64_t s_down_since_us;  // when the station lost (or never had) an IP; 0 = up
static volatile int64_t s_up_since_us;

// Setup hotspot: an open (or CONFIG_APP_SETUP_AP_PASSWORD) access point named after
// the hostname, with a captive-portal DNS, so a phone can join it and set the Wi-Fi
// from the web UI. On when no network is saved, or when the saved one has been
// unreachable for CONFIG_APP_SETUP_AP_AFTER_S; off a minute after the station is back.
#define AP_OFF_AFTER_UP_S 60
// While the hotspot is up, a failed station retries this often instead of at once:
// each attempt takes the radio off the hotspot's channel and scans need it idle.
#define AP_STA_RETRY_MS 20000
static esp_netif_t *s_ap_netif;
static volatile bool s_ap_on;
static volatile bool s_ap_manual;  // started from the console: stays until turned off or a save works
static SemaphoreHandle_t s_ap_lock;

// State of the last wifi_try(), for the web UI to poll.
static SemaphoreHandle_t s_try_lock;
static wifi_try_state_t s_try_state;
static int s_try_attempt;
static char s_try_msg[160];
static atomic_bool s_trying;

static bool is_connected(void)
{
    return xEventGroupGetBits(s_events) & CONNECTED_BIT;
}

static void retry_cb(void *arg)
{
    if (s_sta_configured && !s_hold && !atomic_load(&s_scanning) && !is_connected()) {
        esp_wifi_connect();
    }
}

// Reconnect the station: at once, or paced while the setup hotspot is up.
static void reconnect_soon(void)
{
    esp_timer_stop(s_retry_timer);  // fails harmlessly if not armed
    if (s_ap_on) {
        esp_timer_start_once(s_retry_timer, AP_STA_RETRY_MS * 1000LL);
    } else {
        retry_cb(NULL);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        if (is_connected() || !s_down_since_us) {
            s_down_since_us = esp_timer_get_time();
        }
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        s_ip[0] = '\0';
        if (s_hold || atomic_load(&s_scanning)) {
            return;  // whoever set these reconnects when done
        }
        if (s_retries++ < 5 || s_retries % 20 == 0) {
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", ev->reason);
        }
        reconnect_soon();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "connected, ip %s, http://%s.local/", s_ip, s_hostname);
        s_retries = 0;
        s_down_since_us = 0;
        s_up_since_us = esp_timer_get_time();
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = data;
        ESP_LOGI(TAG, "setup hotspot: " MACSTR " joined", MAC2STR(ev->mac));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = data;
        ESP_LOGI(TAG, "setup hotspot: " MACSTR " left", MAC2STR(ev->mac));
    }
}

// ---- Setup hotspot ----

static esp_err_t ap_start(bool manual)
{
    xSemaphoreTake(s_ap_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (manual) {
        s_ap_manual = true;
    }
    if (!s_ap_on) {
        wifi_config_t cfg = {0};
        size_t n = strnlen(s_hostname, sizeof(cfg.ap.ssid));
        memcpy(cfg.ap.ssid, s_hostname, n);
        cfg.ap.ssid_len = n;
        cfg.ap.channel = 1;  // follows the station's channel once that connects
        cfg.ap.max_connection = 4;
        const char *pw = CONFIG_APP_SETUP_AP_PASSWORD;
        if (strlen(pw) >= 8) {
            strlcpy((char *)cfg.ap.password, pw, sizeof(cfg.ap.password));
            cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            cfg.ap.authmode = WIFI_AUTH_OPEN;
        }
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        }
        if (err == ESP_OK) {
            s_ap_on = true;
            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(s_ap_netif, &ip);
            captive_dns_start(ip.ip.addr, ip.netmask.addr);
            ESP_LOGW(TAG, "setup hotspot on: join Wi-Fi '%s'%s and open http://" IPSTR "/", s_hostname,
                     cfg.ap.authmode == WIFI_AUTH_OPEN ? "" : " (password in the build config)", IP2STR(&ip.ip));
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
            ESP_LOGE(TAG, "setup hotspot failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_ap_lock);
    return err;
}

static esp_err_t ap_stop(void)
{
    xSemaphoreTake(s_ap_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    s_ap_manual = false;
    if (s_ap_on) {
        captive_dns_stop();
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_on = false;
        ESP_LOGI(TAG, "setup hotspot off");
        if (!is_connected()) {
            reconnect_soon();  // back to unpaced retries
        }
    }
    xSemaphoreGive(s_ap_lock);
    return err;
}

esp_err_t wifi_setup_ap(bool on)
{
    return on ? ap_start(true) : ap_stop();
}

void wifi_get_ap_status(wifi_ap_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->active = s_ap_on;
    if (!out->active) {
        return;
    }
    strlcpy(out->ssid, s_hostname, sizeof(out->ssid));
    out->open = strlen(CONFIG_APP_SETUP_AP_PASSWORD) < 8;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    }
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
        out->clients = list.num;
    }
}

uint32_t wifi_ap_ip4(void)
{
    esp_netif_ip_info_t ip;
    if (!s_ap_on || esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK) {
        return 0;
    }
    return ip.ip.addr;
}

// Turns the hotspot on when the station has no way onto a network, and off once
// it has been back for a while.
static void supervise_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        bool busy = atomic_load(&s_trying);
        if (!s_ap_on && !busy) {
            int64_t down = s_down_since_us;
            if (!s_sta_configured) {
                ap_start(false);
            } else if (CONFIG_APP_SETUP_AP_AFTER_S > 0 && down &&
                       now - down > CONFIG_APP_SETUP_AP_AFTER_S * 1000000LL) {
                ESP_LOGW(TAG, "no Wi-Fi for %d s", CONFIG_APP_SETUP_AP_AFTER_S);
                ap_start(false);
            }
        } else if (s_ap_on && !s_ap_manual && !busy && is_connected() &&
                   now - s_up_since_us > AP_OFF_AFTER_UP_S * 1000000LL) {
            ap_stop();
        }
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
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_sta_configured = err == ESP_OK;
    return err;
}

static esp_err_t apply_config(void)
{
    char ssid[SETTINGS_SSID_MAX], pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass) != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi configured: use the setup hotspot '%s' or the USB console: wifi <ssid> <password>",
                 s_hostname);
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
    esp_timer_stop(s_retry_timer);
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
    s_ap_lock = xSemaphoreCreateMutex();
    s_down_since_us = esp_timer_get_time();
    const esp_timer_create_args_t retry = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&retry, &s_retry_timer));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

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

    if (!s_sta_configured) {
        ap_start(false);  // nothing to join: open the hotspot right away
    }
    xTaskCreate(supervise_task, "wifi_sup", 3072, NULL, 3, NULL);
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
        s_ap_manual = false;  // the hotspot did its job: let it time out
        set_try(WIFI_TRY_OK, false, "Connected to '%s' (%s)", ssid, s_ip);
        return ESP_OK;
    }

    if (!have_old) {
        // Nothing to go back to. With the hotspot up, stop so it can scan and try
        // again; otherwise keep trying the new network, unsaved.
        if (s_ap_on) {
            disconnect_quietly();
            s_sta_configured = false;
            s_hold = false;
        }
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
    if (atomic_load(&s_trying) || atomic_exchange(&s_scanning, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    // The driver refuses to scan while the station is connecting (retries), so
    // pause those for the scan.
    bool was_up = is_connected();
    if (!was_up) {
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    atomic_store(&s_scanning, false);
    if (!was_up) {
        reconnect_soon();
    }
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
    out->connected = is_connected();
    out->configured = s_sta_configured;
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
