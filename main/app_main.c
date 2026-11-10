#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "settings.h"
#include "wifi.h"
#include "web.h"
#include "ota.h"
#include "console_cmds.h"
#include "features.h"

static const char *TAG = "app";

void app_main(void)
{
    // Give the host time to re-open the USB-Serial/JTAG port after a reset.
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "%s %s starting", esp_app_get_description()->project_name,
             esp_app_get_description()->version);

    ota_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(wifi_start());
    // After wifi_start(), which creates the default event loop features use.
    features_init();
    ESP_ERROR_CHECK(web_start());
    features_register_routes(web_server());
    // Also registers the features' console commands.
    ESP_ERROR_CHECK(console_start());

    ota_confirm_or_rollback();
}
