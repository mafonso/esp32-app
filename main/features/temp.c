// Chip temperature: owns the internal sensor, keeps a 10-minute history in a
// ring buffer and streams each sample on WebSocket topic "temp". Other code
// reads the current value with temp_get_celsius(). UI: web/features/temp.js.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "web.h"
#include "features.h"
#include "temp.h"

static const char *TAG = "temp";

#define INTERVAL_S 2
#define HISTORY_LEN (10 * 60 / INTERVAL_S)  // 10 minutes

static temperature_sensor_handle_t s_sensor;
static SemaphoreHandle_t s_lock;            // guards s_sensor reads and the ring
static float *s_ring;                       // HISTORY_LEN samples, oldest at s_head when full
static size_t s_head;                       // next write position
static size_t s_count;

esp_err_t temp_get_celsius(float *out)
{
    if (!s_sensor || !s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = temperature_sensor_get_celsius(s_sensor, out);
    xSemaphoreGive(s_lock);
    return err;
}

// In double, so cJSON prints 41.3 rather than 41.29999923706055.
static double round1(float c)
{
    return round(c * 10.0) / 10.0;
}

// i-th sample, oldest first. Caller holds s_lock.
static float ring_at(size_t i)
{
    size_t start = (s_count < HISTORY_LEN) ? 0 : s_head;
    return s_ring[(start + i) % HISTORY_LEN];
}

static void temp_task(void *arg)
{
    for (;;) {
        float c;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = temperature_sensor_get_celsius(s_sensor, &c);
        if (err == ESP_OK) {
            s_ring[s_head] = c;
            s_head = (s_head + 1) % HISTORY_LEN;
            if (s_count < HISTORY_LEN) {
                s_count++;
            }
        }
        xSemaphoreGive(s_lock);

        if (err == ESP_OK && web_ws_client_count() > 0) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "c", round1(c));
            web_ws_broadcast("temp", d);
        }
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_S * 1000));
    }
}

static esp_err_t temp_init(void)
{
    s_ring = heap_caps_malloc(HISTORY_LEN * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        s_ring = malloc(HISTORY_LEN * sizeof(float));
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_lock) {
        return ESP_ERR_NO_MEM;
    }

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    temperature_sensor_handle_t h;
    esp_err_t err = temperature_sensor_install(&cfg, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = temperature_sensor_enable(h);
    if (err != ESP_OK) {
        temperature_sensor_uninstall(h);
        return err;
    }
    s_sensor = h;

    if (xTaskCreate(temp_task, "temp", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sampling every %d s, %d samples of history", INTERVAL_S, HISTORY_LEN);
    return ESP_OK;
}

// {"interval_s":2,"samples":[...]}, oldest first. Built by hand into one
// buffer instead of 300 cJSON nodes.
static esp_err_t history_get(httpd_req_t *req)
{
    const size_t cap = 48 + HISTORY_LEN * 8;  // "-10.0," is at most 6 chars per sample
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    size_t n = snprintf(buf, cap, "{\"interval_s\":%d,\"samples\":[", INTERVAL_S);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_count && n < cap; i++) {
        n += snprintf(buf + n, cap - n, "%s%.1f", i ? "," : "", ring_at(i));
    }
    xSemaphoreGive(s_lock);
    if (n < cap) {
        n += snprintf(buf + n, cap - n, "]}");
    }
    if (n >= cap) {  // can't happen with sane readings, but never send a truncated body
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "history overflow");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static void temp_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/temp/history", .method = HTTP_GET, .handler = history_get},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

static int cmd_temp(int argc, char **argv)
{
    float now;
    if (temp_get_celsius(&now) != ESP_OK) {
        printf("sensor read failed\n");
        return 1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_count;
    float lo = now, hi = now;
    for (size_t i = 0; i < count; i++) {
        float c = ring_at(i);
        lo = fminf(lo, c);
        hi = fmaxf(hi, c);
    }
    xSemaphoreGive(s_lock);
    printf("now %.1f °C, min %.1f, max %.1f over the last %u s\n",
           now, lo, hi, (unsigned)(count * INTERVAL_S));
    return 0;
}

static void temp_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "temp",
        .help = "Chip temperature: now, and min/max over the last 10 minutes",
        .func = cmd_temp,
    };
    esp_console_cmd_register(&cmd);
}

const feature_t temp_feature = {
    .name = "temp",
    .init = temp_init,
    .register_routes = temp_routes,
    .register_console = temp_console,
};
