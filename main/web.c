#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "wifi.h"
#include "ota.h"
#include "web.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static temperature_sensor_handle_t s_temp;

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "unknown";
    }
}

cJSON *web_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    const esp_app_desc_t *app = esp_app_get_description();

    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(fw, "project", app->project_name);
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "build_date", app->date);
    cJSON_AddStringToObject(fw, "build_time", app->time);
    cJSON_AddStringToObject(fw, "idf", esp_get_idf_version());
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(fw, "partition", running->label);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6];
    char mac_str[18];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *board = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(board, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(board, "revision", chip.revision);
    cJSON_AddNumberToObject(board, "cores", chip.cores);
    cJSON_AddNumberToObject(board, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(board, "flash_bytes", flash);
    cJSON_AddNumberToObject(board, "psram_bytes", esp_psram_get_size());
    cJSON_AddNumberToObject(board, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(board, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(board, "heap_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(board, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(board, "reset_reason", reset_reason_name(esp_reset_reason()));
    cJSON_AddStringToObject(board, "mac", mac_str);
    float celsius;
    if (s_temp && temperature_sensor_get_celsius(s_temp, &celsius) == ESP_OK) {
        cJSON_AddNumberToObject(board, "temp_c", celsius);
    }

    wifi_status_t ws;
    wifi_get_status(&ws);
    cJSON *net = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(net, "hostname", wifi_hostname());
    cJSON_AddBoolToObject(net, "connected", ws.connected);
    cJSON_AddStringToObject(net, "ssid", ws.ssid);
    cJSON_AddStringToObject(net, "ip", ws.ip);
    cJSON_AddNumberToObject(net, "rssi", ws.rssi);
    return root;
}

static cJSON *ota_json(void)
{
    ota_status_t st;
    ota_get_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", ota_state_name(st.state));
    cJSON_AddBoolToObject(o, "checked", st.checked);
    cJSON_AddBoolToObject(o, "update_available", st.update_available);
    cJSON_AddStringToObject(o, "running", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "latest", st.latest_version);
    cJSON_AddStringToObject(o, "notes", st.latest_notes);
    cJSON_AddNumberToObject(o, "bytes_read", st.bytes_read);
    cJSON_AddNumberToObject(o, "total_bytes", st.total_bytes);
    cJSON_AddStringToObject(o, "error", st.error);
    return o;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

// State-changing endpoints require a custom header, which cross-site pages
// cannot send without a CORS preflight that this server never approves.
static bool check_csrf(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing X-Requested-With header");
    return false;
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t info_get(httpd_req_t *req)
{
    return send_json(req, web_info_json());
}

static esp_err_t ota_status_get(httpd_req_t *req)
{
    return send_json(req, ota_json());
}

static esp_err_t ota_check_post(httpd_req_t *req)
{
    if (!check_csrf(req)) {
        return ESP_OK;
    }
    ota_check();
    return send_json(req, ota_json());
}

static esp_err_t ota_update_post(httpd_req_t *req)
{
    if (!check_csrf(req)) {
        return ESP_OK;
    }
    if (ota_start_update() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "run a check first, or an update is already running");
        return ESP_OK;
    }
    return send_json(req, ota_json());
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!check_csrf(req)) {
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_start(void)
{
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_temp) == ESP_OK) {
        temperature_sensor_enable(s_temp);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // The OTA check does a TLS handshake inside the request handler.
    cfg.stack_size = 10240;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/api/ota", .method = HTTP_GET, .handler = ota_status_get},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = ota_check_post},
        {.uri = "/api/ota/update", .method = HTTP_POST, .handler = ota_update_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "web server started on port %d", cfg.server_port);
    return ESP_OK;
}
