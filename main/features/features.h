#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// A feature is one .c file in main/features/ that defines
//
//     const feature_t <name>_feature = {.name = "<name>", .init = ..., ...};
//
// and one X(<name>) line in FEATURE_LIST in features.c. Every hook may be NULL.
// See features/sysinfo.c for a complete example and CLAUDE.md for the steps.
typedef struct {
    const char *name;
    // Called once at boot, after Wi-Fi has started (default event loop exists).
    // Create tasks, install drivers, call web_ws_on() and subscribe to APP_EVENT.
    esp_err_t (*init)(void);
    // Register HTTP routes with web_register_routes().
    void (*register_routes)(httpd_handle_t server);
    // Register console commands with esp_console_cmd_register().
    void (*register_console)(void);
} feature_t;

// Run every feature's init(); a failing feature is logged and skipped later.
void features_init(void);
void features_register_routes(httpd_handle_t server);
void features_register_console(void);
