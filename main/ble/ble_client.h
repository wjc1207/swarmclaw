#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_percent;
    bool temperature_valid;
    bool humidity_valid;
} ble_measurement_t;

esp_err_t ble_client_init(void);
bool ble_client_is_connected(void);
esp_err_t ble_client_connect(const char *target_addr, uint32_t timeout_ms);
esp_err_t ble_client_read_measurement(ble_measurement_t *measurement, uint32_t timeout_ms);
esp_err_t ble_client_disconnect(uint32_t timeout_ms);
