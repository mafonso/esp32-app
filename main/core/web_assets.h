#pragma once

#include <stddef.h>
#include <stdint.h>

// One embedded file from main/web/. The table itself is generated at build time
// (build/esp-idf/main/web_assets.c, see main/web_assets.cmake).
typedef struct {
    const char *path;       // URL path, e.g. "/features/sysinfo.js"
    const uint8_t *start;
    const uint8_t *end;
    const char *mime;
} web_asset_t;

extern const web_asset_t web_assets[];
extern const size_t web_assets_count;
