#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_DOWNLOADING,
    OTA_REBOOTING,
    OTA_ERROR,
} ota_state_t;

typedef struct {
    ota_state_t state;
    bool checked;           // a check has completed successfully
    bool update_available;  // latest is newer than the running version
    char latest_version[32];
    char latest_url[256];
    char latest_notes[256];
    int bytes_read;
    int total_bytes;
    char error[96];
} ota_status_t;

void ota_init(void);
const char *ota_state_name(ota_state_t state);

// If this image is pending verification, confirm it once Wi-Fi is up, else roll back.
void ota_confirm_or_rollback(void);

// Fetch the release manifest and compare it with the running version (blocking).
esp_err_t ota_check(void);

// Download and install the version found by the last check, then reboot (async).
esp_err_t ota_start_update(void);

void ota_get_status(ota_status_t *out);
