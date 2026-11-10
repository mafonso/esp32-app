#include "app_events.h"

ESP_EVENT_DEFINE_BASE(APP_EVENT);

esp_err_t app_event_post(app_event_id_t id, const void *data, size_t size)
{
    return esp_event_post(APP_EVENT, id, data, size, 0);
}
