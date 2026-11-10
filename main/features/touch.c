// Capacitive touch on all 14 ESP32-S3 touch channels (GPIO1–14). The hardware
// scans continuously; a 10 Hz task reads each channel's smoothed value and
// benchmark (the hardware's slowly tracking idle level), decides "touched"
// with hysteresis, and streams snapshots on WebSocket topic "touch".
// UI: web/features/touch.js.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "web.h"
#include "features.h"

static const char *TAG = "touch";

#define CHAN_NUM TOUCH_TOTAL_CHAN_NUM  // 14 on the S3: channel n is GPIOn
#define READ_PERIOD_MS 100             // 10 Hz
#define BROADCAST_MIN_MS 200           // periodic snapshots at most 5 Hz
#define INIT_SCANS 3

// Touch threshold, as a rise of the smoothed reading over the benchmark, in
// percent. On touch hardware v2 (S2/S3) readings go UP when a finger adds
// capacitance. Measured on esp-dev: a finger on a bare header pin adds
// +100-300%; idle noise is about 0.2%, but warm-up drift after boot reached +4%
// on one pin. Release below the lower value (hysteresis) so a reading
// hovering at the threshold doesn't flicker. Tune with the `touch` command.
#define TOUCH_ON_PCT 20.0f
#define TOUCH_OFF_PCT 10.0f

// Safety net for drift: the hardware freezes a channel's benchmark while it is
// active, so a slow drift past the threshold would never recover. A pin held
// "touched" this long while reading under DRIFT_MAX_PCT (real touches are far
// higher) gets its benchmark reset to the current reading.
#define DRIFT_HOLD_US (10 * 1000 * 1000)
#define DRIFT_MAX_PCT (2 * TOUCH_ON_PCT)

typedef struct {
    uint8_t gpio;
    uint32_t raw;    // smoothed reading
    int32_t delta;   // raw - benchmark
    bool touched;
} pin_t;

static touch_sensor_handle_t s_sens;
static touch_channel_handle_t s_chan[CHAN_NUM];
static pin_t s_pins[CHAN_NUM];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;  // guards s_pins

static void snapshot(pin_t out[CHAN_NUM])
{
    taskENTER_CRITICAL(&s_lock);
    memcpy(out, s_pins, sizeof(s_pins));
    taskEXIT_CRITICAL(&s_lock);
}

// {"pins":[{gpio,raw,delta,touched},...],"thresh_pct":20}
static cJSON *snapshot_json(void)
{
    pin_t pins[CHAN_NUM];
    snapshot(pins);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "pins");
    for (int i = 0; i < CHAN_NUM; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "gpio", pins[i].gpio);
        cJSON_AddNumberToObject(p, "raw", pins[i].raw);
        cJSON_AddNumberToObject(p, "delta", pins[i].delta);
        cJSON_AddBoolToObject(p, "touched", pins[i].touched);
        cJSON_AddItemToArray(arr, p);
    }
    cJSON_AddNumberToObject(root, "thresh_pct", TOUCH_ON_PCT);
    return root;
}

// Reads every channel and updates s_pins. Returns true if any touched flag
// changed. Only called from one task at a time (init, then touch_task).
static bool update(void)
{
    static int64_t s_since_us[CHAN_NUM];   // when each pin became touched
    static const touch_chan_benchmark_config_t reset = {.do_reset = true};
    int64_t now = esp_timer_get_time();
    bool changed = false;
    for (int i = 0; i < CHAN_NUM; i++) {
        uint32_t smooth, bench;
        if (touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_SMOOTH, &smooth) != ESP_OK ||
            touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, &bench) != ESP_OK) {
            continue;
        }
        int32_t delta = (int32_t)smooth - (int32_t)bench;
        float pct = bench ? 100.0f * delta / bench : 0.0f;
        taskENTER_CRITICAL(&s_lock);
        pin_t *p = &s_pins[i];
        bool was = p->touched;
        p->raw = smooth;
        p->delta = delta;
        p->touched = was ? pct > TOUCH_OFF_PCT : pct > TOUCH_ON_PCT;
        bool touched = p->touched;
        taskEXIT_CRITICAL(&s_lock);

        if (touched && !was) {
            s_since_us[i] = now;
        } else if (touched && now - s_since_us[i] > DRIFT_HOLD_US && pct < DRIFT_MAX_PCT &&
                   touch_channel_config_benchmark(s_chan[i], &reset) == ESP_OK) {
            ESP_LOGW(TAG, "GPIO%u stuck at +%.1f%% for %d s: drift, re-baselined", s_pins[i].gpio,
                     pct, DRIFT_HOLD_US / 1000000);
            taskENTER_CRITICAL(&s_lock);
            p->touched = false;
            p->delta = 0;
            taskEXIT_CRITICAL(&s_lock);
            touched = false;
        }
        changed |= touched != was;
    }
    return changed;
}

static void touch_task(void *arg)
{
    int64_t last_us = 0;
    for (;;) {
        bool changed = update();
        int64_t now = esp_timer_get_time();
        // Touch edges go out at once; otherwise throttle, and only with listeners.
        if (web_ws_client_count() > 0 && (changed || now - last_us >= BROADCAST_MIN_MS * 1000)) {
            web_ws_broadcast("touch", snapshot_json());
            last_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}

static void on_ws_request(cJSON *data)
{
    web_ws_broadcast("touch", snapshot_json());
}

static void cleanup(void)
{
    for (int i = 0; i < CHAN_NUM; i++) {
        if (s_chan[i]) {
            touch_sensor_del_channel(s_chan[i]);
            s_chan[i] = NULL;
        }
    }
    if (s_sens) {
        touch_sensor_del_controller(s_sens);
        s_sens = NULL;
    }
}

// Follows examples/peripherals/touch_sensor/touch_sens_basic: create, filter,
// a few one-shot scans to seed the benchmark, set each channel's hardware
// threshold from it, then scan continuously.
static esp_err_t setup(void)
{
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2),
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(1, sample_cfg);
    esp_err_t err = touch_sensor_new_controller(&sens_cfg, &s_sens);
    if (err != ESP_OK) {
        return err;
    }

    // Placeholder threshold; replaced below once the benchmark is known.
    touch_channel_config_t chan_cfg = {
        .active_thresh = {2000},
        .charge_speed = TOUCH_CHARGE_SPEED_7,
        .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT,
    };
    for (int i = 0; i < CHAN_NUM; i++) {
        err = touch_sensor_new_channel(s_sens, TOUCH_MIN_CHAN_ID + i, &chan_cfg, &s_chan[i]);
        if (err != ESP_OK) {
            return err;
        }
        touch_chan_info_t info = {0};
        err = touch_sensor_get_channel_info(s_chan[i], &info);
        if (err != ESP_OK) {
            return err;
        }
        s_pins[i].gpio = info.chan_gpio;
    }

    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    // Slow benchmark tracking: follows temperature drift, not fingers.
    filter_cfg.benchmark.filter_mode = TOUCH_BM_IIR_FILTER_16;
    err = touch_sensor_config_filter(s_sens, &filter_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = touch_sensor_enable(s_sens);
    for (int i = 0; i < INIT_SCANS && err == ESP_OK; i++) {
        err = touch_sensor_trigger_oneshot_scanning(s_sens, 2000);
    }
    esp_err_t err2 = touch_sensor_disable(s_sens);
    if (err != ESP_OK || err2 != ESP_OK) {
        return err != ESP_OK ? err : err2;
    }

    // The hardware's own active state stops benchmark updates while touched,
    // so give it the same ON threshold as the software decision: drift below
    // it keeps being tracked, a real touch freezes the benchmark.
    for (int i = 0; i < CHAN_NUM; i++) {
        uint32_t bench = 0;
        err = touch_channel_read_data(s_chan[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, &bench);
        if (err != ESP_OK) {
            return err;
        }
        uint32_t thresh = (uint32_t)(bench * TOUCH_ON_PCT / 100.0f);
        chan_cfg.active_thresh[0] = thresh ? thresh : 1;
        err = touch_sensor_reconfig_channel(s_chan[i], &chan_cfg);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = touch_sensor_enable(s_sens);
    if (err != ESP_OK) {
        return err;
    }
    err = touch_sensor_start_continuous_scanning(s_sens);
    if (err != ESP_OK) {
        touch_sensor_disable(s_sens);
    }
    return err;
}

static esp_err_t touch_init(void)
{
    esp_err_t err = setup();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch sensor unavailable: %s", esp_err_to_name(err));
        cleanup();
        return err;
    }
    update();
    web_ws_on("touch", on_ws_request);
    if (xTaskCreate(touch_task, "touch", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%d channels (GPIO%d–%d), threshold +%.1f%%", CHAN_NUM,
             s_pins[0].gpio, s_pins[CHAN_NUM - 1].gpio, TOUCH_ON_PCT);
    return ESP_OK;
}

static esp_err_t touch_get(httpd_req_t *req)
{
    return web_send_json(req, snapshot_json());
}

static void touch_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/touch", .method = HTTP_GET, .handler = touch_get},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

static int cmd_touch(int argc, char **argv)
{
    pin_t pins[CHAN_NUM];
    snapshot(pins);
    printf("GPIO      raw  benchmark   delta      %%  touched\n");
    for (int i = 0; i < CHAN_NUM; i++) {
        uint32_t bench = pins[i].raw - pins[i].delta;
        float pct = bench ? 100.0f * pins[i].delta / bench : 0.0f;
        printf("%4u %8lu %10lu %7ld %6.1f  %s\n", pins[i].gpio, (unsigned long)pins[i].raw,
               (unsigned long)bench, (long)pins[i].delta, pct, pins[i].touched ? "yes" : "");
    }
    printf("touched above +%.1f%%, released below +%.1f%%\n", TOUCH_ON_PCT, TOUCH_OFF_PCT);
    return 0;
}

static void touch_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "touch",
        .help = "Touch pin readings (GPIO1-14)",
        .func = cmd_touch,
    };
    esp_console_cmd_register(&cmd);
}

const feature_t touch_feature = {
    .name = "touch",
    .init = touch_init,
    .register_routes = touch_routes,
    .register_console = touch_console,
};
