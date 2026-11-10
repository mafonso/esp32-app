#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    int rssi;
} wifi_status_t;

typedef enum {
    WIFI_TRY_IDLE,
    WIFI_TRY_TRYING,
    WIFI_TRY_OK,
    WIFI_TRY_FAILED,
} wifi_try_state_t;

// Start the station using credentials from settings, plus mDNS.
esp_err_t wifi_start(void);

// Reconnect with new credentials (already saved to settings by the caller).
esp_err_t wifi_reconnect(void);

// Block until an IP is obtained or the timeout expires.
bool wifi_wait_connected(int timeout_ms);

// Switch networks without stranding the board: connect with these credentials
// (not saved yet) and save them only if an IP arrives within timeout_ms;
// otherwise go back to the saved network. Blocks. ESP_ERR_TIMEOUT on failure,
// ESP_ERR_INVALID_STATE if a try is already running, ESP_ERR_INVALID_ARG for a
// bad SSID or a password of 1-7 characters.
esp_err_t wifi_try(const char *ssid, const char *pass, int timeout_ms);

// The same in a background task, for requests that arrive over this very
// Wi-Fi. *attempt identifies this try in wifi_try_status().
esp_err_t wifi_try_async(const char *ssid, const char *pass, int timeout_ms, int *attempt);
void wifi_try_status(wifi_try_state_t *state, int *attempt, char *msg, size_t msg_len);
const char *wifi_try_state_name(wifi_try_state_t state);

// Blocking scan (a few seconds). On success *out holds *count records (at most
// 40) for the caller to free(). ESP_ERR_INVALID_STATE while a try is running.
esp_err_t wifi_scan(wifi_ap_record_t **out, uint16_t *count);

void wifi_get_status(wifi_status_t *out);
const char *wifi_hostname(void);

// "<prefix>-xxyyzz" from the MAC, used when no name is saved.
void wifi_default_hostname(char *buf, size_t len);
