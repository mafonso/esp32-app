#pragma once

#include "esp_err.h"

// Current chip temperature in °C. Returns ESP_ERR_INVALID_STATE if the sensor
// isn't available (the temp feature failed or hasn't initialised yet).
esp_err_t temp_get_celsius(float *out);
