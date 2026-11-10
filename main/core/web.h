#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

// Start the HTTP server: core API routes, the /ws hub and the static web UI.
esp_err_t web_start(void);

// The running server, or NULL before web_start().
httpd_handle_t web_server(void);

// Board/firmware/network stats, shared by the web API and the console.
cJSON *web_info_json(void);

// ---- Helpers for feature routes ----

// Register extra routes. Always use this rather than httpd_register_uri_handler():
// it keeps the static-file wildcard last so exact routes win. Strings in
// `routes` are copied, so the array may live on the stack.
esp_err_t web_register_routes(const httpd_uri_t *routes, size_t n);

// Send `json` as the response (takes ownership and frees it).
esp_err_t web_send_json(httpd_req_t *req, cJSON *json);

// Admin-only routes also call web_require_admin(req) (auth.h) right after this.
// For state-changing requests: true if the X-Requested-With header is present,
// else sends 403 and returns false (the handler should then return ESP_OK).
bool web_check_csrf(httpd_req_t *req);

// Read a JSON request body of at most max_len bytes. On failure sends 400 and
// returns NULL (the handler should then return ESP_OK). Caller frees the result.
cJSON *web_read_json(httpd_req_t *req, size_t max_len);

// ---- WebSocket hub (/ws) ----
// Every message, in both directions, is a JSON text frame {"t": topic, "d": data}.

// Send {"t":topic,"d":data} to every connected browser. Takes ownership of
// `data` (NULL sends null). Safe from any task; never blocks: the message is
// queued for the server task, and dropped if the queue is full (16 KB total)
// or the message is over 4 KB. Never logs, so it is safe from a log hook.
// Batch bursts (e.g. many log lines) into one message.
esp_err_t web_ws_broadcast(const char *topic, cJSON *data);

// Call `cb` when a browser sends a message with this topic. `data` is the "d"
// member (may be NULL) and is only valid during the call; do not free it.
// Callbacks run on the HTTP server task, so keep them short.
// Register during feature init (up to 16 handlers in total).
esp_err_t web_ws_on(const char *topic, void (*cb)(cJSON *data));

// Number of open WebSocket clients; skip sampling work when it is 0.
int web_ws_client_count(void);

// Internal: called by web_start() to register /ws.
esp_err_t web_ws_init(httpd_handle_t server);

// Restart the board in about half a second, after the current response is sent.
void web_reboot_soon(void);

// Internal: called by web_start() to register the settings/admin API (web_settings.c).
esp_err_t web_settings_init(void);
