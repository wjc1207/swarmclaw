#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Unified GPIO tool — single entry point for all hardware I/O:
 *   GPIO raw pin control, I²C, SPI, RGB/WS2812B, PWM, UART, 1-Wire.
 *
 * Input JSON must contain an "action" string that selects the operation.
 * All other parameters are passed as sibling keys.
 *
 * Returns JSON: {"ok":true,"result":...} or {"ok":false,"error":"..."}
 */
esp_err_t tool_gpio_execute(const char *input_json,
                            char *output,
                            size_t output_size);

/**
 * Initialize the GPIO tool subsystem (mutex, pin registry).
 * Safe to call multiple times.
 */
esp_err_t tool_gpio_init(void);

/**
 * Restore persisted RGB state from SPIFFS (called at boot).
 * Replaces the former read_rgb_from_file_and_apply().
 */
esp_err_t tool_gpio_rgb_restore(void);
