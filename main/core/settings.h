#pragma once

#include <stddef.h>
#include "esp_err.h"

#define SETTINGS_SSID_MAX 33
#define SETTINGS_PASS_MAX 65
#define SETTINGS_NAME_MAX 32

// Initialise NVS and seed Wi-Fi credentials from Kconfig on first boot.
esp_err_t settings_init(void);

esp_err_t settings_get_wifi(char ssid[SETTINGS_SSID_MAX], char pass[SETTINGS_PASS_MAX]);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);

// Per-board hostname (DHCP + mDNS); ESP_ERR_NVS_NOT_FOUND if unset.
esp_err_t settings_get_name(char name[SETTINGS_NAME_MAX]);
esp_err_t settings_set_name(const char *name);
