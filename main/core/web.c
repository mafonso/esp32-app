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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "wifi.h"
#include "ota.h"
#include "temp.h"
#include "web.h"
#include "web_assets.h"

static const char *TAG = "web";

#ifndef CONFIG_HTTPD_WS_SUPPORT
#error "CONFIG_HTTPD_WS_SUPPORT is off: delete sdkconfig so sdkconfig.defaults applies, then rebuild"
#endif

// Sockets for browsers (page loads plus one WebSocket per tab); LWIP_MAX_SOCKETS
// in sdkconfig.defaults must be at least this + 3.
#define WEB_MAX_SOCKETS 12

static httpd_handle_t s_server;

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
    if (temp_get_celsius(&celsius) == ESP_OK) {  // absent if the temp feature failed
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

esp_err_t web_send_json(httpd_req_t *req, cJSON *json)
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
bool web_check_csrf(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing X-Requested-With header");
    return false;
}

cJSON *web_read_json(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0 || req->content_len > max_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a JSON body");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }
    return json;
}

// Every file under main/web/ is embedded at build time (see web_assets.cmake).
static esp_err_t static_get(httpd_req_t *req)
{
    char path[128];
    size_t len = strcspn(req->uri, "?#");
    if (len >= sizeof(path)) {
        len = 0;  // too long to be one of ours: 404
    }
    memcpy(path, req->uri, len);
    path[len] = '\0';
    if (strcmp(path, "/") == 0) {
        strcpy(path, "/index.html");
    }
    for (size_t i = 0; i < web_assets_count; i++) {
        const web_asset_t *a = &web_assets[i];
        if (strcmp(a->path, path) == 0) {
            httpd_resp_set_type(req, a->mime);
            httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
        }
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

static const httpd_uri_t s_static_route = {.uri = "/*", .method = HTTP_GET, .handler = static_get};

httpd_handle_t web_server(void)
{
    return s_server;
}

esp_err_t web_register_routes(const httpd_uri_t *routes, size_t n)
{
    if (!s_server) {
        return ESP_ERR_INVALID_STATE;
    }
    // httpd matches in registration order and rejects URIs an existing wildcard
    // already covers, so take the static wildcard out and put it back last.
    httpd_unregister_uri_handler(s_server, s_static_route.uri, s_static_route.method);
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "route %s: %s", routes[i].uri, esp_err_to_name(err));
            ret = err;
        }
    }
    httpd_register_uri_handler(s_server, &s_static_route);
    return ret;
}

static esp_err_t info_get(httpd_req_t *req)
{
    return web_send_json(req, web_info_json());
}

static esp_err_t ota_status_get(httpd_req_t *req)
{
    return web_send_json(req, ota_json());
}

static esp_err_t ota_check_post(httpd_req_t *req)
{
    if (!web_check_csrf(req)) {
        return ESP_OK;
    }
    ota_check();
    return web_send_json(req, ota_json());
}

static esp_err_t ota_update_post(httpd_req_t *req)
{
    if (!web_check_csrf(req)) {
        return ESP_OK;
    }
    if (ota_start_update() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "run a check first, or an update is already running");
        return ESP_OK;
    }
    return web_send_json(req, ota_json());
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!web_check_csrf(req)) {
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // The OTA check does a TLS handshake inside the request handler.
    cfg.stack_size = 10240;
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = WEB_MAX_SOCKETS;
    // Reuse the oldest idle connection instead of refusing new ones.
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/api/ota", .method = HTTP_GET, .handler = ota_status_get},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = ota_check_post},
        {.uri = "/api/ota/update", .method = HTTP_POST, .handler = ota_update_post},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
    };
    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    ESP_ERROR_CHECK(web_ws_init(s_server));
    httpd_register_uri_handler(s_server, &s_static_route);
    ESP_LOGI(TAG, "web server started on port %d", cfg.server_port);
    return ESP_OK;
}
