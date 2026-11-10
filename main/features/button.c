// The BOOT button (GPIO0, active low). An any-edge interrupt wakes a task that
// debounces, counts presses and detects long presses, then tells the rest of
// the firmware through APP_EVENT_BUTTON and browsers through topic 'button'.
// Other features react to the event (led.c cycles its mode); this file knows
// nothing about them.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "cJSON.h"
#include "app_events.h"
#include "web.h"
#include "features.h"

static const char *TAG = "button";

// GPIO0 is a strapping pin (held low at reset = download mode): only read it.
#define BUTTON_GPIO   GPIO_NUM_0
#define DEBOUNCE_MS   30
#define LONG_PRESS_MS 1000

static TaskHandle_t s_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_button_event_t s_state;   // guarded by s_lock

static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static app_button_event_t get_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    app_button_event_t st = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

static cJSON *state_json(void)
{
    app_button_event_t st = get_state();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "pressed", st.pressed);
    cJSON_AddNumberToObject(j, "count", st.count);
    cJSON_AddBoolToObject(j, "long_press", st.long_press);
    return j;
}

// Update the state, then tell other features and browsers.
static void report(bool pressed, bool long_press, bool new_press)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.pressed = pressed;
    s_state.long_press = long_press;
    if (new_press) {
        s_state.count++;
    }
    app_button_event_t ev = s_state;
    taskEXIT_CRITICAL(&s_lock);

    if (app_event_post(APP_EVENT_BUTTON, &ev, sizeof(ev)) != ESP_OK) {
        ESP_LOGW(TAG, "event queue full, press dropped");
    }
    web_ws_broadcast("button", state_json());
}

static void button_task(void *arg)
{
    bool pressed = gpio_get_level(BUTTON_GPIO) == 0;
    bool long_sent = false;
    TickType_t pressed_at = 0;

    for (;;) {
        // While held, wake up when the long-press time is reached.
        TickType_t wait = portMAX_DELAY;
        if (pressed && !long_sent) {
            TickType_t held = xTaskGetTickCount() - pressed_at;
            TickType_t need = pdMS_TO_TICKS(LONG_PRESS_MS);
            wait = held >= need ? 0 : need - held;
        }
        if (ulTaskNotifyTake(pdTRUE, wait) > 0) {
            // An edge: wait until the contacts have been quiet for DEBOUNCE_MS.
            while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DEBOUNCE_MS)) > 0) {
            }
            bool now = gpio_get_level(BUTTON_GPIO) == 0;
            if (now == pressed) {
                continue;   // a glitch, or a bounce back to the same level
            }
            pressed = now;
            if (pressed) {
                pressed_at = xTaskGetTickCount();
                long_sent = false;
                report(true, false, true);
            } else {
                report(false, long_sent, false);
            }
        } else if (pressed && !long_sent) {
            long_sent = true;
            report(true, true, false);
        }
    }
}

static void on_ws_request(cJSON *data)
{
    web_ws_broadcast("button", state_json());
}

static esp_err_t button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    // The task reads the pin at start, so create it after the pull-up is on
    // and before the ISR that notifies it.
    if (xTaskCreate(button_task, "button", 3072, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    // Another feature may have installed the shared ISR service already.
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }
    web_ws_on("button", on_ws_request);
    ESP_LOGI(TAG, "BOOT button on GPIO%d", BUTTON_GPIO);
    return ESP_OK;
}

static esp_err_t button_get(httpd_req_t *req)
{
    return web_send_json(req, state_json());
}

static void button_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/button", .method = HTTP_GET, .handler = button_get},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

const feature_t button_feature = {
    .name = "button",
    .init = button_init,
    .register_routes = button_routes,
};
