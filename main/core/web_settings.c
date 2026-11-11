// Settings and admin API: Wi-Fi, hostname, admin password and the log buffer.
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "settings.h"
#include "wifi.h"
#include "auth.h"
#include "logbuf.h"
#include "web.h"

#define WIFI_TRY_TIMEOUT_MS 30000
#define LOGS_MAX_BYTES 8192

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_OWE: return "owe";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "enterprise";
    default: return "other";
    }
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// ---- Wi-Fi ----

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_ap_record_t *aps;
    uint16_t n;
    esp_err_t err = wifi_scan(&aps, &n);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_custom_err(req, "409 Conflict", "a Wi-Fi change is in progress");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        // e.g. the station is busy reconnecting
        httpd_resp_send_custom_err(req, "503 Service Unavailable", "scan failed, try again");
        return ESP_OK;
    }
    // The driver returns records strongest first; keep the first of each SSID.
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) {
            continue;  // hidden network
        }
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(ssid, (const char *)aps[j].ssid) == 0;
        }
        if (seen) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(o, "channel", aps[i].primary);
        cJSON_AddStringToObject(o, "auth", auth_name(aps[i].authmode));
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);
    return web_send_json(req, arr);
}

static esp_err_t wifi_settings_get(httpd_req_t *req)
{
    char ssid[SETTINGS_SSID_MAX] = "", pass[SETTINGS_PASS_MAX];
    if (settings_get_wifi(ssid, pass) != ESP_OK) {
        ssid[0] = '\0';
    }
    memset(pass, 0, sizeof(pass));
    wifi_try_state_t state;
    int attempt;
    char msg[160];
    wifi_try_status(&state, &attempt, msg, sizeof(msg));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddStringToObject(o, "state", wifi_try_state_name(state));
    cJSON_AddNumberToObject(o, "attempt", attempt);
    cJSON_AddStringToObject(o, "message", msg);
    return web_send_json(req, o);
}

static esp_err_t wifi_settings_post(httpd_req_t *req)
{
    if (!web_check_csrf(req) || !web_require_admin(req)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (!body) {
        return ESP_OK;
    }
    const char *ssid = json_str(body, "ssid");
    const char *pass = json_str(body, "password");
    int attempt = 0;
    esp_err_t err = ssid ? wifi_try_async(ssid, pass ? pass : "", WIFI_TRY_TIMEOUT_MS, &attempt)
                         : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "SSID must be 1-32 characters; password empty (open network) or 8-63 characters");
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send_custom_err(req, "409 Conflict", "a Wi-Fi change is already in progress");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "trying");
    cJSON_AddNumberToObject(o, "attempt", attempt);
    cJSON_AddNumberToObject(o, "timeout_s", WIFI_TRY_TIMEOUT_MS / 1000);
    httpd_resp_set_status(req, "202 Accepted");
    return web_send_json(req, o);
}

// ---- Hostname ----

static esp_err_t name_get(httpd_req_t *req)
{
    char def[SETTINGS_NAME_MAX];
    wifi_default_hostname(def, sizeof(def));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", wifi_hostname());
    cJSON_AddStringToObject(o, "default", def);
    return web_send_json(req, o);
}

static esp_err_t name_post(httpd_req_t *req)
{
    if (!web_check_csrf(req) || !web_require_admin(req)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 128);
    if (!body) {
        return ESP_OK;
    }
    char name[SETTINGS_NAME_MAX] = "";
    const char *v = json_str(body, "name");
    bool valid = v && strlen(v) < sizeof(name);
    if (valid) {
        strlcpy(name, v, sizeof(name));
    }
    cJSON_Delete(body);
    if (!valid || (name[0] && !settings_valid_hostname(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use letters, digits and '-', up to 31 characters");
        return ESP_OK;
    }
    char def[SETTINGS_NAME_MAX];
    wifi_default_hostname(def, sizeof(def));
    if (strcasecmp(name, def) == 0) {
        name[0] = '\0';  // same as the default: just clear the override
    }
    const char *effective = name[0] ? name : def;
    bool changed = strcasecmp(effective, wifi_hostname()) != 0;
    esp_err_t err = settings_set_name(name);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", effective);
    cJSON_AddBoolToObject(o, "reboot", changed);
    err = web_send_json(req, o);
    if (changed) {
        web_reboot_soon();
    }
    return err;
}

// ---- Admin password ----

static cJSON *password_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", auth_enabled());
    cJSON_AddStringToObject(o, "user", AUTH_USER);
    return o;
}

static esp_err_t password_get(httpd_req_t *req)
{
    return web_send_json(req, password_json());
}

// Adds a fresh session token, so the page that set the password stays signed in.
static void add_token(cJSON *o)
{
    char tok[AUTH_TOKEN_HEX_LEN + 1];
    if (auth_new_token(tok) == ESP_OK) {
        cJSON_AddStringToObject(o, "token", tok);
    }
}

// The page's sign-in: trades the password for a session token (see auth.h).
static esp_err_t login_post(httpd_req_t *req)
{
    if (!web_check_csrf(req)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (!body) {
        return ESP_OK;
    }
    const char *pw = json_str(body, "password");
    auth_login_t r = auth_enabled() ? auth_login(req, pw ? pw : "") : AUTH_LOGIN_OK;
    cJSON_Delete(body);
    if (r == AUTH_LOGIN_REPLIED) {
        return ESP_OK;
    }
    if (r == AUTH_LOGIN_WRONG) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "wrong password");
        return ESP_OK;
    }
    cJSON *o = password_json();
    add_token(o);
    return web_send_json(req, o);
}

static esp_err_t password_post(httpd_req_t *req)
{
    if (!web_check_csrf(req) || !web_require_admin(req)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 256);
    if (!body) {
        return ESP_OK;
    }
    const char *pw = json_str(body, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (pw && strlen(pw) <= AUTH_PASS_MAX && (pw[0] == '\0' || strlen(pw) >= 4)) {
        err = auth_set_password(pw);
    }
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password must be 4-64 characters (empty disables it)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    cJSON *o = password_json();
    add_token(o);
    return web_send_json(req, o);
}

// ---- Logs ----

// Random per boot, so a client can tell that sequence numbers started over.
static uint32_t s_boot_id;

static void add_line(const char *line, size_t len, void *ctx)
{
    char buf[LOGBUF_LINE_MAX + 1];
    memcpy(buf, line, len);
    buf[len] = '\0';
    cJSON_AddItemToArray((cJSON *)ctx, cJSON_CreateString(buf));
}

static esp_err_t logs_get(httpd_req_t *req)
{
    if (!web_require_admin(req)) {
        return ESP_OK;
    }
    uint32_t after = 0;
    char query[48], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
        after = strtoul(val, NULL, 10);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *lines = cJSON_AddArrayToObject(o, "lines");
    uint32_t last;
    bool dropped;
    logbuf_read(after, LOGS_MAX_BYTES, &last, &dropped, add_line, lines);
    cJSON_AddNumberToObject(o, "seq", last);
    cJSON_AddBoolToObject(o, "dropped", dropped);
    while (!s_boot_id) {
        s_boot_id = esp_random() & 0x7fffffff;
    }
    cJSON_AddNumberToObject(o, "boot", s_boot_id);
    return web_send_json(req, o);
}

esp_err_t web_settings_init(void)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get},
        {.uri = "/api/settings/wifi", .method = HTTP_GET, .handler = wifi_settings_get},
        {.uri = "/api/settings/wifi", .method = HTTP_POST, .handler = wifi_settings_post},
        {.uri = "/api/settings/name", .method = HTTP_GET, .handler = name_get},
        {.uri = "/api/settings/name", .method = HTTP_POST, .handler = name_post},
        {.uri = "/api/settings/password", .method = HTTP_GET, .handler = password_get},
        {.uri = "/api/settings/password", .method = HTTP_POST, .handler = password_post},
        {.uri = "/api/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_get},
    };
    return web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}
