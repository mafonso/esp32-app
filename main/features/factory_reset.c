// Factory reset: erase the NVS partition (Wi-Fi network, hostname, admin
// password, feature settings) and reboot. The firmware is kept. Three ways in:
// POST /api/factory-reset (admin), the 'factory-reset' console command, and
// holding the BOOT button for HOLD_MS (through APP_EVENT_BUTTON, so button.c
// knows nothing about this file).
//
// After the reboot the board has no network saved, so it opens its setup
// hotspot; a build with sdkconfig.secrets seeds that network again instead.
#include <string.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "app_events.h"
#include "auth.h"
#include "web.h"
#include "features.h"

static const char *TAG = "factory_reset";

#define HOLD_MS 10000

static esp_timer_handle_t s_hold_timer;

// Erase NVS under its own lock (open handles are closed, later NVS calls fail
// until the reboot). Doesn't restart, so callers can report the result first.
static esp_err_t erase_settings(void)
{
    ESP_LOGW(TAG, "erasing all settings");
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void on_hold_timer(void *arg)
{
    ESP_LOGW(TAG, "BOOT held for %d s: factory reset", HOLD_MS / 1000);
    erase_settings();
    vTaskDelay(pdMS_TO_TICKS(200));   // let the log line reach the console
    esp_restart();
}

static void on_button(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const app_button_event_t *ev = data;
    if (ev->pressed && !ev->long_press) {
        esp_timer_stop(s_hold_timer);   // not running is fine
        esp_timer_start_once(s_hold_timer, (uint64_t)HOLD_MS * 1000);
    } else if (!ev->pressed) {
        esp_timer_stop(s_hold_timer);
    }
}

static esp_err_t factory_reset_init(void)
{
    const esp_timer_create_args_t args = {.callback = on_hold_timer, .name = "factory_reset"};
    esp_err_t err = esp_timer_create(&args, &s_hold_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_register(APP_EVENT, APP_EVENT_BUTTON, on_button, NULL);
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!web_check_csrf(req) || !web_require_admin(req)) {
        return ESP_OK;
    }
    esp_err_t err = erase_settings();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    err = web_send_json(req, o);
    web_reboot_soon();
    return err;
}

static void factory_reset_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_post},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("erases the Wi-Fi network, hostname, admin password and all settings, then reboots\n"
               "usage: factory-reset yes\n");
        return 1;
    }
    esp_err_t err = erase_settings();
    if (err != ESP_OK) {
        printf("erase failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("settings erased, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static void factory_reset_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "factory-reset",
        .help = "Erase all settings (Wi-Fi, hostname, admin password) and reboot",
        .hint = "yes",
        .func = cmd_factory_reset,
    };
    esp_console_cmd_register(&cmd);
}

const feature_t factory_reset_feature = {
    .name = "factory_reset",
    .init = factory_reset_init,
    .register_routes = factory_reset_routes,
    .register_console = factory_reset_console,
};
