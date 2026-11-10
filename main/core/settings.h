#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SETTINGS_SSID_MAX 33
#define SETTINGS_PASS_MAX 65
#define SETTINGS_NAME_MAX 32
#define SETTINGS_SALT_LEN 16
#define SETTINGS_HASH_LEN 32

// Initialise NVS and seed Wi-Fi credentials from Kconfig on first boot.
esp_err_t settings_init(void);

esp_err_t settings_get_wifi(char ssid[SETTINGS_SSID_MAX], char pass[SETTINGS_PASS_MAX]);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);

// Per-board hostname (DHCP + mDNS); ESP_ERR_NVS_NOT_FOUND if unset.
esp_err_t settings_get_name(char name[SETTINGS_NAME_MAX]);
esp_err_t settings_set_name(const char *name);

// Letters, digits and '-', not starting or ending with '-', 1..SETTINGS_NAME_MAX-1 chars.
bool settings_valid_hostname(const char *name);

// Admin password as a salted SHA-256; ESP_ERR_NVS_NOT_FOUND if unset.
// settings_set_admin_hash(NULL, NULL) removes it.
esp_err_t settings_get_admin_hash(uint8_t salt[SETTINGS_SALT_LEN], uint8_t hash[SETTINGS_HASH_LEN]);
esp_err_t settings_set_admin_hash(const uint8_t salt[SETTINGS_SALT_LEN], const uint8_t hash[SETTINGS_HASH_LEN]);
