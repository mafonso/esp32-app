#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "web.h"

static const char *TAG = "ws";

#define WS_HANDLERS_MAX 16
#define WS_RX_MAX 1024          // larger incoming frames close the connection
#define WS_TX_MSG_MAX 4096      // larger outgoing messages are dropped
#define WS_TX_QUEUE_BYTES 16384 // bound on memory held by queued messages
#define WS_TX_QUEUE_LEN 32
#define WS_CLIENTS_MAX 16

typedef struct {
    const char *topic;
    void (*cb)(cJSON *data);
} ws_handler_t;

static httpd_handle_t s_server;
static ws_handler_t s_handlers[WS_HANDLERS_MAX];
static int s_handler_count;

// Outgoing messages wait here for the server task; one work item drains them all.
static QueueHandle_t s_txq;
static atomic_size_t s_tx_bytes;
static atomic_bool s_tx_scheduled;

static size_t ws_clients(int *fds, size_t max)
{
    size_t n = WS_CLIENTS_MAX;
    int all[WS_CLIENTS_MAX];
    if (!s_server || httpd_get_client_list(s_server, &n, all) != ESP_OK) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, all[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            if (fds && count < max) {
                fds[count] = all[i];
            }
            count++;
        }
    }
    return count;
}

int web_ws_client_count(void)
{
    return ws_clients(NULL, 0);
}

// Runs on the server task. Must not log: a log hook may broadcast.
static void ws_send_work(void *arg)
{
    // Clear first, so a message queued while we drain schedules another pass.
    atomic_store(&s_tx_scheduled, false);
    char *msg;
    while (xQueueReceive(s_txq, &msg, 0) == pdTRUE) {
        size_t len = strlen(msg);
        int fds[WS_CLIENTS_MAX];
        size_t n = ws_clients(fds, WS_CLIENTS_MAX);
        for (size_t i = 0; i < n && i < WS_CLIENTS_MAX; i++) {
            httpd_ws_frame_t frame = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)msg,
                .len = len,
            };
            if (httpd_ws_send_frame_async(s_server, fds[i], &frame) != ESP_OK) {
                httpd_sess_trigger_close(s_server, fds[i]);
            }
        }
        atomic_fetch_sub(&s_tx_bytes, len);
        free(msg);
    }
}

esp_err_t web_ws_broadcast(const char *topic, cJSON *data)
{
    if (!s_txq || web_ws_client_count() == 0) {
        cJSON_Delete(data);
        return s_txq ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "t", topic);
    cJSON_AddItemToObject(root, "d", data ? data : cJSON_CreateNull());
    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!msg) {
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(msg);
    if (len > WS_TX_MSG_MAX || atomic_fetch_add(&s_tx_bytes, len) + len > WS_TX_QUEUE_BYTES) {
        if (len <= WS_TX_MSG_MAX) {
            atomic_fetch_sub(&s_tx_bytes, len);
        }
        free(msg);
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(s_txq, &msg, 0) != pdTRUE) {
        atomic_fetch_sub(&s_tx_bytes, len);
        free(msg);
        return ESP_ERR_NO_MEM;
    }
    // If scheduling fails the message stays queued and goes out with the next one.
    if (!atomic_exchange(&s_tx_scheduled, true) &&
        httpd_queue_work(s_server, ws_send_work, NULL) != ESP_OK) {
        atomic_store(&s_tx_scheduled, false);
    }
    return ESP_OK;
}

esp_err_t web_ws_on(const char *topic, void (*cb)(cJSON *data))
{
    if (s_handler_count >= WS_HANDLERS_MAX) {
        ESP_LOGE(TAG, "too many handlers, '%s' not registered", topic);
        return ESP_ERR_NO_MEM;
    }
    s_handlers[s_handler_count++] = (ws_handler_t){.topic = topic, .cb = cb};
    return ESP_OK;
}

static void dispatch(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    const cJSON *t = cJSON_GetObjectItem(root, "t");
    if (cJSON_IsString(t)) {
        cJSON *d = cJSON_GetObjectItem(root, "d");
        for (int i = 0; i < s_handler_count; i++) {
            if (strcmp(s_handlers[i].topic, t->valuestring) == 0) {
                s_handlers[i].cb(d);
            }
        }
    }
    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK;  // handshake done
    }
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.len > WS_RX_MAX) {
        return ESP_FAIL;  // closes the connection
    }
    char *buf = malloc(frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = (uint8_t *)buf;
    err = frame.len ? httpd_ws_recv_frame(req, &frame, frame.len) : ESP_OK;
    if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
        buf[frame.len] = '\0';
        dispatch(buf);
    }
    free(buf);
    return err;
}

esp_err_t web_ws_init(httpd_handle_t server)
{
    s_server = server;
    s_txq = xQueueCreate(WS_TX_QUEUE_LEN, sizeof(char *));
    if (!s_txq) {
        return ESP_ERR_NO_MEM;
    }
    static const httpd_uri_t route = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    return httpd_register_uri_handler(server, &route);
}
