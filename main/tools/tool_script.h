#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Write a Python script to SPIFFS.
 * Input JSON: {"path": "/spiffs/scripts/...", "content": "..."}
 * Path must start with /spiffs/scripts/ and must not contain "..".
 */
esp_err_t tool_script_write_execute(const char *input_json,
                                    char *output, size_t output_size);

/**
 * Execute a Python script from SPIFFS and return captured output.
 * Input JSON: {"path": "/spiffs/scripts/...", "timeout_ms": 5000}
 */
esp_err_t tool_script_run_execute(const char *input_json,
                                  char *output, size_t output_size);
