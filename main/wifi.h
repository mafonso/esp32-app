#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool connected;
    char ssid[33];
    char ip[16];
    int rssi;
} wifi_status_t;

// Start the station using credentials from settings, plus mDNS.
esp_err_t wifi_start(void);

// Reconnect with new credentials (already saved to settings by the caller).
esp_err_t wifi_reconnect(void);

// Block until an IP is obtained or the timeout expires.
bool wifi_wait_connected(int timeout_ms);

void wifi_get_status(wifi_status_t *out);
const char *wifi_hostname(void);
