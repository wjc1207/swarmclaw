#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a MicroPython script from SPIFFS.
 *
 * Creates a fresh MicroPython VM per invocation (heap allocated from PSRAM),
 * registers hardware modules, redirects print() to a capture buffer,
 * and runs the file at @p script_path.
 *
 * @param script_path  Absolute path, e.g. "/spiffs/scripts/blink.py"
 * @param timeout_ms   Maximum execution time (enforced via FreeRTOS watchdog)
 * @param out_buf      On return, heap-allocated string with captured output
 *                     (caller must free).  On error contains the error message.
 * @return ESP_OK on success, ESP_FAIL on MicroPython error
 */
esp_err_t mpy_runner_exec(const char *script_path, int timeout_ms,
                          char **out_buf);
