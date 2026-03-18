#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_percent;
    int battery_percent;
    bool temperature_valid;
    bool humidity_valid;
    bool battery_valid;
} ble_measurement_t;

#define BLE_RAW_MAX_RECORDS 24
#define BLE_RAW_MAX_VALUE_LEN 32

typedef struct {
    uint16_t service_uuid;
    uint16_t characteristic_uuid;
    uint16_t value_handle;
    uint16_t raw_len;
    uint8_t raw[BLE_RAW_MAX_VALUE_LEN];
    bool valid;
} ble_raw_data_t;

esp_err_t ble_client_init(void);
bool ble_client_is_connected(void);
esp_err_t ble_client_connect(const char *target_addr, uint32_t timeout_ms);
esp_err_t ble_client_read_measurement(ble_measurement_t *measurement, uint32_t timeout_ms);
esp_err_t ble_client_read_all_raw(ble_raw_data_t *out_records, size_t max_records,
                                  size_t *out_count, uint32_t timeout_ms);
esp_err_t ble_client_disconnect(uint32_t timeout_ms);
