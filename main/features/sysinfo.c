// Example feature, and the template to copy for new ones. It shows each hook:
// a task that streams live data over the WebSocket, an HTTP route, an incoming
// WebSocket topic and a console command. Its UI is web/features/sysinfo.js.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"
#include "cJSON.h"
#include "web.h"
#include "features.h"

static const char *TAG = "sysinfo";

#define PERIOD_MS 2000

// The "board" part of /api/info: uptime, heap, PSRAM, temperature, ...
static cJSON *sample(void)
{
    cJSON *info = web_info_json();
    cJSON *board = cJSON_DetachItemFromObject(info, "board");
    cJSON_Delete(info);
    return board;
}

static void sysinfo_task(void *arg)
{
    for (;;) {
        // Only do the work when a browser is listening.
        if (web_ws_client_count() > 0) {
            web_ws_broadcast("sysinfo", sample());
        }
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

// A browser sends {"t":"sysinfo"} when its card opens, to get data right away.
static void on_ws_request(cJSON *data)
{
    web_ws_broadcast("sysinfo", sample());
}

static esp_err_t sysinfo_init(void)
{
    web_ws_on("sysinfo", on_ws_request);
    if (xTaskCreate(sysinfo_task, "sysinfo", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "streaming every %d ms", PERIOD_MS);
    return ESP_OK;
}

static esp_err_t sysinfo_get(httpd_req_t *req)
{
    return web_send_json(req, sample());
}

static void sysinfo_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/sysinfo", .method = HTTP_GET, .handler = sysinfo_get},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

static int cmd_sysinfo(int argc, char **argv)
{
    cJSON *board = sample();
    char *s = cJSON_Print(board);
    printf("%s\n", s);
    cJSON_free(s);
    cJSON_Delete(board);
    return 0;
}

static void sysinfo_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "sysinfo",
        .help = "Live board stats (uptime, heap, temperature)",
        .func = cmd_sysinfo,
    };
    esp_console_cmd_register(&cmd);
}

const feature_t sysinfo_feature = {
    .name = "sysinfo",
    .init = sysinfo_init,
    .register_routes = sysinfo_routes,
    .register_console = sysinfo_console,
};
