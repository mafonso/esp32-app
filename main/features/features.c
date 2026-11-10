#include <stdbool.h>
#include <stddef.h>
#include "esp_log.h"
#include "features.h"

static const char *TAG = "features";

// One line per feature, in alphabetical order. Each X(name) refers to the
// `const feature_t name_feature` defined in features/name.c.
#define FEATURE_LIST(X) \
    X(sysinfo)          \
    /* end of list */

#define DECLARE(name) extern const feature_t name##_feature;
FEATURE_LIST(DECLARE)

#define ENTRY(name) &name##_feature,
static const feature_t *const s_features[] = {FEATURE_LIST(ENTRY)};

#define FEATURE_COUNT (sizeof(s_features) / sizeof(s_features[0]))

static bool s_failed[FEATURE_COUNT];

void features_init(void)
{
    for (size_t i = 0; i < FEATURE_COUNT; i++) {
        const feature_t *f = s_features[i];
        esp_err_t err = f->init ? f->init() : ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: init failed: %s", f->name, esp_err_to_name(err));
            s_failed[i] = true;
        }
    }
}

void features_register_routes(httpd_handle_t server)
{
    for (size_t i = 0; i < FEATURE_COUNT; i++) {
        if (!s_failed[i] && s_features[i]->register_routes) {
            s_features[i]->register_routes(server);
        }
    }
}

void features_register_console(void)
{
    for (size_t i = 0; i < FEATURE_COUNT; i++) {
        if (!s_failed[i] && s_features[i]->register_console) {
            s_features[i]->register_console();
        }
    }
}
