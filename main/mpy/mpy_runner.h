#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a Python script from SPIFFS using the embedded MicroPython interpreter.
 *
 * Creates a fresh interpreter state per invocation (heap allocated from PSRAM),
 * executes the script, captures all print() output, then tears down cleanly.
 *
 * @param script_path  Absolute path, e.g. "/spiffs/scripts/blink.py"
 * @param timeout_ms   Maximum execution time (enforced via FreeRTOS watchdog)
 * @param out_buf      On return, heap-allocated string with captured output
 *                     (caller must free).  On error contains the error message.
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t mpy_runner_exec(const char *script_path, int timeout_ms,
                          char **out_buf);
