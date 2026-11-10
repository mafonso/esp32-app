#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "wifi.h"
#include "ota.h"

static const char *TAG = "ota";

#define MANIFEST_MAX 2048
// GitHub redirects to signed asset URLs with long query strings.
#define HTTP_BUF_SIZE 4096

static ota_status_t s_status;
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void set_error(const char *fmt, esp_err_t err)
{
    lock();
    s_status.state = OTA_ERROR;
    snprintf(s_status.error, sizeof(s_status.error), "%s: %s", fmt, esp_err_to_name(err));
    unlock();
    ESP_LOGE(TAG, "%s", s_status.error);
}

const char *ota_state_name(ota_state_t state)
{
    static const char *names[] = {"idle", "checking", "downloading", "rebooting", "error"};
    return names[state];
}

void ota_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void ota_get_status(ota_status_t *out)
{
    lock();
    *out = s_status;
    unlock();
}

// Compare "vMAJOR.MINOR.PATCH[-suffix]" strings numerically; suffixes are ignored.
static int version_cmp(const char *a, const char *b)
{
    int va[3] = {0}, vb[3] = {0};
    sscanf(a + (*a == 'v'), "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b + (*b == 'v'), "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] < vb[i] ? -1 : 1;
        }
    }
    return 0;
}

void ota_confirm_or_rollback(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGI(TAG, "new image pending verification, waiting for Wi-Fi");
    if (wifi_wait_connected(60000)) {
        ESP_LOGI(TAG, "Wi-Fi OK, marking image valid");
        esp_ota_mark_app_valid_cancel_rollback();
    } else {
        ESP_LOGE(TAG, "no Wi-Fi after update, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

typedef struct {
    char *buf;
    int len;
} body_t;

static esp_err_t on_http_event(esp_http_client_event_t *ev)
{
    body_t *body = ev->user_data;
    // Redirect responses carry their own bodies; only keep the final 200 body.
    if (ev->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(ev->client) == 200) {
        int n = ev->data_len;
        if (body->len + n >= MANIFEST_MAX) {
            n = MANIFEST_MAX - 1 - body->len;
        }
        memcpy(body->buf + body->len, ev->data, n);
        body->len += n;
    }
    return ESP_OK;
}

esp_err_t ota_check(void)
{
    lock();
    if (s_status.state == OTA_CHECKING || s_status.state == OTA_DOWNLOADING ||
        s_status.state == OTA_REBOOTING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = OTA_CHECKING;
    s_status.error[0] = '\0';
    unlock();

    body_t body = {.buf = calloc(1, MANIFEST_MAX)};
    if (!body.buf) {
        set_error("out of memory", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t cfg = {
        .url = CONFIG_APP_OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_data = &body,
        .buffer_size = HTTP_BUF_SIZE,
        .buffer_size_tx = HTTP_BUF_SIZE,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && code != 200) {
        ESP_LOGE(TAG, "manifest HTTP status %d", code);
        err = code == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (err != ESP_OK) {
        free(body.buf);
        set_error("manifest fetch failed", err);
        return err;
    }

    cJSON *root = cJSON_Parse(body.buf);
    free(body.buf);
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    const cJSON *notes = cJSON_GetObjectItem(root, "notes");
    if (!cJSON_IsString(ver) || !cJSON_IsString(url)) {
        cJSON_Delete(root);
        set_error("invalid manifest", ESP_ERR_INVALID_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *running = esp_app_get_description()->version;
    lock();
    strlcpy(s_status.latest_version, ver->valuestring, sizeof(s_status.latest_version));
    strlcpy(s_status.latest_url, url->valuestring, sizeof(s_status.latest_url));
    strlcpy(s_status.latest_notes, cJSON_IsString(notes) ? notes->valuestring : "",
            sizeof(s_status.latest_notes));
    s_status.update_available = version_cmp(s_status.latest_version, running) > 0;
    s_status.checked = true;
    s_status.state = OTA_IDLE;
    ESP_LOGI(TAG, "latest %s, running %s, update %s", s_status.latest_version, running,
             s_status.update_available ? "available" : "not needed");
    unlock();
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t validate_image(esp_https_ota_handle_t handle)
{
    esp_app_desc_t incoming;
    esp_err_t err = esp_https_ota_get_img_desc(handle, &incoming);
    if (err != ESP_OK) {
        return err;
    }
    const esp_app_desc_t *self = esp_app_get_description();
    if (strcmp(incoming.project_name, self->project_name) != 0) {
        ESP_LOGE(TAG, "image is for project '%s', expected '%s'", incoming.project_name, self->project_name);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_LOGI(TAG, "installing %s (built %s %s)", incoming.version, incoming.date, incoming.time);
    return ESP_OK;
}

static void update_task(void *arg)
{
    char url[256];
    lock();
    strlcpy(url, s_status.latest_url, sizeof(url));
    unlock();

    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_BUF_SIZE,
        .buffer_size_tx = HTTP_BUF_SIZE,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t handle = NULL;

    esp_err_t err = esp_https_ota_begin(&cfg, &handle);
    if (err == ESP_OK) {
        err = validate_image(handle);
    }
    if (err == ESP_OK) {
        lock();
        s_status.total_bytes = esp_https_ota_get_image_size(handle);
        unlock();
        while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            lock();
            s_status.bytes_read = esp_https_ota_get_image_len_read(handle);
            unlock();
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = esp_https_ota_finish(handle);
    } else if (handle) {
        esp_https_ota_abort(handle);
    }

    if (err == ESP_OK) {
        lock();
        s_status.bytes_read = s_status.total_bytes;
        s_status.state = OTA_REBOOTING;
        unlock();
        ESP_LOGI(TAG, "update installed, rebooting");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    set_error("update failed", err);
    vTaskDelete(NULL);
}

esp_err_t ota_start_update(void)
{
    lock();
    bool busy = s_status.state == OTA_CHECKING || s_status.state == OTA_DOWNLOADING ||
                s_status.state == OTA_REBOOTING;
    if (busy || !s_status.checked || !s_status.latest_url[0]) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_status.state = OTA_DOWNLOADING;
    s_status.bytes_read = 0;
    s_status.total_bytes = 0;
    s_status.error[0] = '\0';
    unlock();

    if (xTaskCreate(update_task, "ota", 8192, NULL, 5, NULL) != pdPASS) {
        set_error("task create failed", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
