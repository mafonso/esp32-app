// The onboard WS2812 RGB LED: an effects task (off, solid, breathe, blink,
// rainbow) with colour, brightness and speed, controlled over HTTP, the
// WebSocket topic 'led' and the console. A short BOOT button press cycles the
// mode via APP_EVENT_BUTTON. State and the LED GPIO are kept in NVS.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "led_strip.h"
#include "app_events.h"
#include "web.h"
#include "features.h"

static const char *TAG = "led";
static const char *NS = "app";

#define FRAME_MS      20      // 50 Hz while animating
#define SAVE_DELAY_US 2000000 // write NVS 2 s after the last change, to spare flash

typedef enum { MODE_OFF, MODE_SOLID, MODE_BREATHE, MODE_BLINK, MODE_RAINBOW, MODE_COUNT } led_mode_t;
static const char *const MODE_NAMES[MODE_COUNT] = {"off", "solid", "breathe", "blink", "rainbow"};

// The LED is on GPIO48 on DevKitC-1 v1.0 boards and GPIO38 on v1.1.
static const int GPIO_CHOICES[] = {48, 38};

typedef struct {
    uint8_t mode;
    uint32_t color;       // 0xRRGGBB
    uint8_t brightness;   // 0-100
    uint8_t speed;        // 1-10
    uint8_t gpio;
} led_state_t;

// A requested change; -1 means "leave as is".
typedef struct {
    int mode, brightness, speed, gpio;
    int32_t color;
} led_patch_t;

#define PATCH_NONE {.mode = -1, .brightness = -1, .speed = -1, .gpio = -1, .color = -1}

static SemaphoreHandle_t s_lock;
static led_state_t s_state = {
    .mode = MODE_BREATHE, .color = 0x2080ff, .brightness = 20, .speed = 5, .gpio = 48,
};
static bool s_save_pending;   // guarded by s_lock
static TaskHandle_t s_task;

static led_state_t get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    led_state_t st = s_state;
    xSemaphoreGive(s_lock);
    return st;
}

// ---- NVS ----

static void load_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;   // nothing saved yet
    }
    uint8_t u8;
    uint32_t u32;
    if (nvs_get_u8(h, "led_gpio", &u8) == ESP_OK && (u8 == 48 || u8 == 38)) {
        s_state.gpio = u8;
    }
    if (nvs_get_u8(h, "led_mode", &u8) == ESP_OK && u8 < MODE_COUNT) {
        s_state.mode = u8;
    }
    if (nvs_get_u32(h, "led_color", &u32) == ESP_OK && u32 <= 0xffffff) {
        s_state.color = u32;
    }
    if (nvs_get_u8(h, "led_bright", &u8) == ESP_OK && u8 <= 100) {
        s_state.brightness = u8;
    }
    if (nvs_get_u8(h, "led_speed", &u8) == ESP_OK && u8 >= 1 && u8 <= 10) {
        s_state.speed = u8;
    }
    nvs_close(h);
}

// Mode, colour, brightness, speed (debounced by the task).
static void save_state(const led_state_t *st)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "led_mode", st->mode);
        if (err == ESP_OK) {
            err = nvs_set_u32(h, "led_color", st->color);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "led_bright", st->brightness);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "led_speed", st->speed);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving state failed: %s", esp_err_to_name(err));
    }
}

// The GPIO is a setting: saved right away, so a reboot just after keeps it.
static esp_err_t save_gpio(uint8_t gpio)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "led_gpio", gpio);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ---- Effects ----

static void hue_to_rgb(uint32_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    hue %= 360;
    uint32_t sector = hue / 60, f = (hue % 60) * 255 / 60;
    uint8_t up = f, down = 255 - f;
    switch (sector) {
    case 0: *r = 255; *g = up; *b = 0; break;
    case 1: *r = down; *g = 255; *b = 0; break;
    case 2: *r = 0; *g = 255; *b = up; break;
    case 3: *r = 0; *g = down; *b = 255; break;
    case 4: *r = up; *g = 0; *b = 255; break;
    default: *r = 255; *g = 0; *b = down; break;
    }
}

// The colour to show at time now_ms, as 0xRRGGBB.
static uint32_t frame(const led_state_t *st, int64_t now_ms)
{
    uint8_t r = st->color >> 16, g = st->color >> 8, b = st->color;
    float level = 1.0f;   // 0..1, multiplied by the brightness

    switch (st->mode) {
    case MODE_OFF:
        return 0;
    case MODE_BREATHE: {
        int period = 5500 - 500 * st->speed;   // 5 s .. 0.5 s
        float phase = (float)(now_ms % period) / period;
        float s = (1.0f - cosf(phase * 2.0f * (float)M_PI)) / 2.0f;
        level = s * s;   // squared looks more even to the eye
        break;
    }
    case MODE_BLINK: {
        int period = 2200 - 200 * st->speed;   // 2 s .. 0.2 s
        level = (now_ms % period) < period / 2 ? 1.0f : 0.0f;
        break;
    }
    case MODE_RAINBOW: {
        int period = 11000 - 1000 * st->speed; // 10 s .. 1 s per colour wheel
        hue_to_rgb((uint32_t)((now_ms % period) * 360 / period), &r, &g, &b);
        break;
    }
    default:
        break;
    }
    float k = level * st->brightness / 100.0f;
    return ((uint32_t)(r * k + 0.5f) << 16) | ((uint32_t)(g * k + 0.5f) << 8) | (uint32_t)(b * k + 0.5f);
}

static led_strip_handle_t open_strip(int gpio)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED on GPIO%d: %s", gpio, esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "LED on GPIO%d", gpio);
    return strip;
}

// Owns the led_strip device: other tasks change s_state and notify this one.
static void led_task(void *arg)
{
    led_strip_handle_t strip = NULL;
    int strip_gpio = -1;
    uint32_t shown = UINT32_MAX;
    int64_t save_at = 0;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        led_state_t st = s_state;
        bool changed = s_save_pending;
        s_save_pending = false;
        xSemaphoreGive(s_lock);

        int64_t now_us = esp_timer_get_time();
        if (changed) {
            save_at = now_us + SAVE_DELAY_US;
        }

        if (st.gpio != strip_gpio) {
            if (strip) {
                led_strip_clear(strip);   // don't leave the old pin lit
                led_strip_del(strip);
            }
            strip = open_strip(st.gpio);
            strip_gpio = st.gpio;
            shown = UINT32_MAX;
        }

        uint32_t rgb = frame(&st, now_us / 1000);
        if (strip && rgb != shown) {
            esp_err_t err = led_strip_set_pixel(strip, 0, rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff);
            if (err == ESP_OK) {
                err = led_strip_refresh(strip);
            }
            shown = err == ESP_OK ? rgb : UINT32_MAX;
        }

        if (save_at && now_us >= save_at) {
            save_state(&st);
            save_at = 0;
        }

        // Sleep until the next frame, the pending save, or a change.
        TickType_t wait = portMAX_DELAY;
        if (st.mode == MODE_BREATHE || st.mode == MODE_BLINK || st.mode == MODE_RAINBOW) {
            wait = pdMS_TO_TICKS(FRAME_MS);
        }
        if (save_at) {
            TickType_t until_save = pdMS_TO_TICKS((save_at - now_us) / 1000) + 1;
            if (until_save < wait) {
                wait = until_save;
            }
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

// ---- State changes ----

static cJSON *state_json(void)
{
    led_state_t st = get_state();
    char color[8];
    snprintf(color, sizeof(color), "#%06lx", (unsigned long)st.color);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", MODE_NAMES[st.mode]);
    cJSON_AddStringToObject(j, "color", color);
    cJSON_AddNumberToObject(j, "brightness", st.brightness);
    cJSON_AddNumberToObject(j, "speed", st.speed);
    cJSON_AddNumberToObject(j, "gpio", st.gpio);
    return j;
}

static int parse_mode(const char *s)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        if (strcmp(s, MODE_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

// "#rrggbb" -> 0xRRGGBB, or -1.
static int32_t parse_color(const char *s)
{
    if (strlen(s) != 7 || s[0] != '#') {
        return -1;
    }
    for (int i = 1; i < 7; i++) {
        if (!isxdigit((unsigned char)s[i])) {
            return -1;
        }
    }
    return (int32_t)strtoul(s + 1, NULL, 16);
}

static bool valid_gpio(int gpio)
{
    for (size_t i = 0; i < sizeof(GPIO_CHOICES) / sizeof(GPIO_CHOICES[0]); i++) {
        if (gpio == GPIO_CHOICES[i]) {
            return true;
        }
    }
    return false;
}

// An integer member in [lo, hi]; false if present but invalid.
static bool get_int(const cJSON *obj, const char *key, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) {
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (int)v->valuedouble ||
        v->valuedouble < lo || v->valuedouble > hi) {
        return false;
    }
    *out = (int)v->valuedouble;
    return true;
}

// Parse a POST body / WebSocket message. Returns an error message, or NULL.
static const char *parse_patch(const cJSON *d, led_patch_t *p)
{
    if (!cJSON_IsObject(d)) {
        return "expected a JSON object";
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, "mode");
    if (v && (!cJSON_IsString(v) || (p->mode = parse_mode(v->valuestring)) < 0)) {
        return "mode must be off, solid, breathe, blink or rainbow";
    }
    v = cJSON_GetObjectItemCaseSensitive(d, "color");
    if (v && (!cJSON_IsString(v) || (p->color = parse_color(v->valuestring)) < 0)) {
        return "color must be #rrggbb";
    }
    if (!get_int(d, "brightness", 0, 100, &p->brightness)) {
        return "brightness must be an integer 0-100";
    }
    if (!get_int(d, "speed", 1, 10, &p->speed)) {
        return "speed must be an integer 1-10";
    }
    if (!get_int(d, "gpio", 0, 255, &p->gpio) || (p->gpio >= 0 && !valid_gpio(p->gpio))) {
        return "gpio must be 48 or 38";
    }
    return NULL;
}

// Apply a validated patch, wake the task and tell every browser.
static esp_err_t apply_patch(const led_patch_t *p)
{
    esp_err_t err = ESP_OK;
    if (p->gpio >= 0) {
        err = save_gpio(p->gpio);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "saving GPIO failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (p->mode >= 0) {
        s_state.mode = p->mode;
    }
    if (p->color >= 0) {
        s_state.color = p->color;
    }
    if (p->brightness >= 0) {
        s_state.brightness = p->brightness;
    }
    if (p->speed >= 0) {
        s_state.speed = p->speed;
    }
    if (p->gpio >= 0) {
        s_state.gpio = p->gpio;
    }
    s_save_pending = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    web_ws_broadcast("led", state_json());
    return err;
}

static void on_ws(cJSON *d)
{
    led_patch_t p = PATCH_NONE;
    // An empty message asks for the current state; so does an invalid one,
    // which puts the sender's controls back.
    if (!d || cJSON_IsNull(d) || parse_patch(d, &p) != NULL) {
        web_ws_broadcast("led", state_json());
        return;
    }
    apply_patch(&p);
}

// A short BOOT press (released before the long-press time) cycles the mode.
static void on_button(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const app_button_event_t *ev = data;
    if (ev->pressed || ev->long_press) {
        return;
    }
    led_patch_t p = PATCH_NONE;
    p.mode = (get_state().mode + 1) % MODE_COUNT;
    apply_patch(&p);
}

static esp_err_t led_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    load_state();
    if (xTaskCreate(led_task, "led", 3072, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    web_ws_on("led", on_ws);
    esp_err_t err = esp_event_handler_register(APP_EVENT, APP_EVENT_BUTTON, on_button, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "button events unavailable: %s", esp_err_to_name(err));
    }
    led_state_t st = get_state();
    ESP_LOGI(TAG, "%s #%06lx at %d%%", MODE_NAMES[st.mode], (unsigned long)st.color, st.brightness);
    return ESP_OK;
}

// ---- HTTP ----

static esp_err_t led_get(httpd_req_t *req)
{
    return web_send_json(req, state_json());
}

static esp_err_t led_post(httpd_req_t *req)
{
    if (!web_check_csrf(req)) {
        return ESP_OK;
    }
    cJSON *body = web_read_json(req, 512);
    if (!body) {
        return ESP_OK;
    }
    led_patch_t p = PATCH_NONE;
    const char *msg = parse_patch(body, &p);
    cJSON_Delete(body);
    if (msg) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
    }
    if (apply_patch(&p) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "saving the GPIO failed");
    }
    return web_send_json(req, state_json());
}

static void led_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/led", .method = HTTP_GET, .handler = led_get},
        {.uri = "/api/led", .method = HTTP_POST, .handler = led_post},
    };
    web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
}

// ---- Console ----

static void print_state(void)
{
    led_state_t st = get_state();
    printf("mode %s, color #%06lx, brightness %d, speed %d, GPIO%d\n", MODE_NAMES[st.mode],
           (unsigned long)st.color, st.brightness, st.speed, st.gpio);
}

static int cmd_led(int argc, char **argv)
{
    led_patch_t p = PATCH_NONE;
    if (argc >= 2 && strcmp(argv[1], "gpio") == 0) {
        p.gpio = argc == 3 ? atoi(argv[2]) : -1;
        if (!valid_gpio(p.gpio)) {
            printf("usage: led gpio <48|38>\n");
            return 1;
        }
    } else if (argc >= 2) {
        p.mode = parse_mode(argv[1]);
        for (int i = 2; i < argc && p.mode >= 0; i++) {
            char *end;
            long n = strtol(argv[i], &end, 10);
            if (argv[i][0] == '#' && (p.color = parse_color(argv[i])) >= 0) {
                continue;
            }
            if (*argv[i] && !*end && n >= 0 && n <= 100) {
                p.brightness = n;
                continue;
            }
            p.mode = -1;   // bad argument
        }
        if (p.mode < 0) {
            printf("usage: led [off|solid|breathe|blink|rainbow] [#rrggbb] [brightness 0-100]\n"
                   "       led gpio <48|38>\n");
            return 1;
        }
    }
    if (argc >= 2) {
        apply_patch(&p);
    }
    print_state();
    return 0;
}

static void led_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "led",
        .help = "Show or set the RGB LED: led [mode] [#rrggbb] [brightness], led gpio <48|38>",
        .func = cmd_led,
    };
    esp_console_cmd_register(&cmd);
}

const feature_t led_feature = {
    .name = "led",
    .init = led_init,
    .register_routes = led_routes,
    .register_console = led_console,
};
