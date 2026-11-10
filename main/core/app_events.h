#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

// Application events on the default event loop, so features can react to each
// other without calling each other directly:
//
//     esp_event_handler_register(APP_EVENT, APP_EVENT_BUTTON, on_button, NULL);
//     app_event_post(APP_EVENT_BUTTON, &ev, sizeof(ev));
//
// Event data is copied by the loop. Handlers run on the event loop task: keep
// them short and never block. Add new IDs at the end of the enum.
ESP_EVENT_DECLARE_BASE(APP_EVENT);

typedef enum {
    APP_EVENT_BUTTON,   // reserved for the BOOT button feature (PR 2)
} app_event_id_t;

// Post to the default loop without blocking; fails if the loop queue is full.
esp_err_t app_event_post(app_event_id_t id, const void *data, size_t size);
