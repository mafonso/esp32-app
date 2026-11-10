#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t web_start(void);

// Board/firmware/network stats, shared by the web API and the console.
cJSON *web_info_json(void);
