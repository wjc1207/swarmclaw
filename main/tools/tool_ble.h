#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t tool_ble_execute(const char *input_json, char *output, size_t output_size);
