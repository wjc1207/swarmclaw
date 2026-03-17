#include "ble/ble_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_client";

#define DEFAULT_CONNECT_TIMEOUT_MS 15000
#define DEFAULT_READ_TIMEOUT_MS     5000

#define EVT_SYNCED          BIT0
#define EVT_LISTEN_FAILED   BIT1
#define EVT_MEAS_READY      BIT2

#define BTHOME_UUID16_LO 0xD2
#define BTHOME_UUID16_HI 0xFC

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static bool s_started;
static bool s_listening;
static uint8_t s_own_addr_type;
static char s_target_addr[18];
static bool s_addr_filter_enabled;

static ble_measurement_t s_last_measurement;

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_addr_to_str(const ble_addr_t *addr, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
}

static int bthome_obj_len(uint8_t id)
{
    switch (id) {
    case 0x01: return 1; /* battery u8 */
    case 0x02: return 2; /* temperature s16, 0.01 C */
    case 0x03: return 2; /* humidity u16, 0.01 % */
    case 0x08: return 2;
    case 0x09: return 2;
    case 0x0A: return 1;
    case 0x0B: return 2;
    case 0x10: return 1;
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 2;
    case 0x14: return 1;
    case 0x15: return 1;
    case 0x16: return 1;
    case 0x17: return 1;
    case 0x18: return 1;
    default:   return -1;
    }
}

static bool ble_parse_bthome_service_data(const uint8_t *svc, uint8_t svc_len, ble_measurement_t *out)
{
    if (svc == NULL || out == NULL || svc_len < 3) {
        return false;
    }

    if (svc[0] != BTHOME_UUID16_LO || svc[1] != BTHOME_UUID16_HI) {
        return false;
    }

    uint8_t devinfo = svc[2];
    if ((devinfo & 0x01U) != 0) {
        /* Encrypted BTHome payload cannot be decoded without key support. */
        return false;
    }

    ble_measurement_t parsed = {0};
    size_t i = 3;
    while (i < svc_len) {
        uint8_t obj_id = svc[i++];
        int len = bthome_obj_len(obj_id);
        if (len <= 0 || i + (size_t)len > svc_len) {
            break;
        }

        if (obj_id == 0x02 && len == 2) {
            int16_t raw = (int16_t)((uint16_t)svc[i] | ((uint16_t)svc[i + 1] << 8));
            parsed.temperature_c = (float)raw / 100.0f;
            parsed.temperature_valid = true;
        } else if (obj_id == 0x03 && len == 2) {
            uint16_t raw = (uint16_t)svc[i] | ((uint16_t)svc[i + 1] << 8);
            parsed.humidity_percent = (float)raw / 100.0f;
            parsed.humidity_valid = true;
        }

        i += (size_t)len;
    }

    if (!parsed.temperature_valid && !parsed.humidity_valid) {
        return false;
    }

    *out = parsed;
    return true;
}

static bool ble_parse_bthome_adv(const uint8_t *adv_data, uint8_t adv_len, ble_measurement_t *out)
{
    if (adv_data == NULL || out == NULL) {
        return false;
    }

    size_t i = 0;
    while (i + 1 < adv_len) {
        uint8_t field_len = adv_data[i];
        if (field_len == 0) {
            break;
        }

        size_t field_total = (size_t)field_len + 1;
        if (i + field_total > adv_len) {
            break;
        }

        uint8_t ad_type = adv_data[i + 1];
        if (ad_type == 0x16 && field_len >= 3) {
            const uint8_t *svc = &adv_data[i + 2];
            uint8_t svc_len = (uint8_t)(field_len - 1);
            if (ble_parse_bthome_service_data(svc, svc_len, out)) {
                return true;
            }
        }

        i += field_total;
    }

    return false;
}

static bool ble_parse_bthome_adv_minimal(const uint8_t *adv_data, uint8_t adv_len, ble_measurement_t *out)
{
    if (adv_data == NULL || out == NULL || adv_len < 4) {
        return false;
    }

    /* Minimal detector: AD type 0x16 + BTHome UUID 0xFCD2 (little-endian: D2 FC). */
    for (size_t i = 0; i + 2 < adv_len; i++) {
        if (adv_data[i] == 0x16 && adv_data[i + 1] == BTHOME_UUID16_LO && adv_data[i + 2] == BTHOME_UUID16_HI) {
            return ble_parse_bthome_service_data(&adv_data[i + 1], (uint8_t)(adv_len - (i + 1)), out);
        }
    }

    return false;
}

static esp_err_t ble_start_scan_locked(void)
{
    const struct ble_gap_disc_params scan_params = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &scan_params, ble_gap_event, NULL);
    if (rc == BLE_HS_EALREADY) {
        return ESP_OK;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char addr_str[18] = {0};
        ble_addr_to_str(&event->disc.addr, addr_str, sizeof(addr_str));
        if (s_addr_filter_enabled && strcasecmp(addr_str, s_target_addr) != 0) {
            return 0;
        }

        ble_measurement_t measurement = {0};
        if (ble_parse_bthome_adv_minimal(event->disc.data, event->disc.length_data, &measurement) ||
            ble_parse_bthome_adv(event->disc.data, event->disc.length_data, &measurement)) {
            s_last_measurement = measurement;
            xEventGroupSetBits(s_events, EVT_MEAS_READY);
            ESP_LOGI(TAG, "BTHome v2 adv from %s temp_valid=%d hum_valid=%d", addr_str,
                     measurement.temperature_valid, measurement.humidity_valid);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_listening) {
            (void)ble_start_scan_locked();
        }
        return 0;
    default:
        return 0;
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset reason=%d", reason);
    s_listening = false;
    xEventGroupClearBits(s_events, EVT_SYNCED | EVT_MEAS_READY);
    xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
        return;
    }

    xEventGroupSetBits(s_events, EVT_SYNCED);
    ESP_LOGI(TAG, "BLE host synced");
}

esp_err_t ble_client_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = nimble_port_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "nimble_port_init failed");
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(ble_host_task);
    s_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_SYNCED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(5000));
    if ((bits & EVT_SYNCED) == 0) {
        ESP_LOGE(TAG, "BLE host sync timed out");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool ble_client_is_connected(void)
{
    return s_listening;
}

esp_err_t ble_client_connect(const char *target_addr, uint32_t timeout_ms)
{
    if (target_addr == NULL || target_addr[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ble_client_init(), TAG, "ble_client_init failed");

    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_CONNECT_TIMEOUT_MS;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    strlcpy(s_target_addr, target_addr, sizeof(s_target_addr));
    s_addr_filter_enabled = true;
    s_listening = true;
    s_last_measurement.temperature_valid = false;
    s_last_measurement.humidity_valid = false;

    xEventGroupClearBits(s_events, EVT_LISTEN_FAILED | EVT_MEAS_READY);

    esp_err_t start_err = ble_start_scan_locked();
    if (start_err != ESP_OK) {
        s_listening = false;
        xSemaphoreGive(s_mutex);
        return start_err;
    }

    xSemaphoreGive(s_mutex);

    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_MEAS_READY | EVT_LISTEN_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & EVT_MEAS_READY) {
        return ESP_OK;
    }

    (void)ble_client_disconnect(1000);
    return (bits & EVT_LISTEN_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t ble_client_read_measurement(ble_measurement_t *measurement, uint32_t timeout_ms)
{
    if (measurement == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_listening) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_READ_TIMEOUT_MS;
    }

    if (s_last_measurement.temperature_valid || s_last_measurement.humidity_valid) {
        *measurement = s_last_measurement;
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_MEAS_READY | EVT_LISTEN_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & EVT_MEAS_READY) {
        *measurement = s_last_measurement;
        return ESP_OK;
    }

    return (bits & EVT_LISTEN_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t ble_client_disconnect(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_READ_TIMEOUT_MS;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_listening = false;
    s_addr_filter_enabled = false;
    s_target_addr[0] = '\0';
    s_last_measurement.temperature_valid = false;
    s_last_measurement.humidity_valid = false;
    xEventGroupClearBits(s_events, EVT_MEAS_READY | EVT_LISTEN_FAILED);

    int rc = ble_gap_disc_cancel();
    xSemaphoreGive(s_mutex);

    if (rc == 0 || rc == BLE_HS_EALREADY || rc == BLE_HS_EBUSY || rc == BLE_HS_EINVAL) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "ble_gap_disc_cancel rc=%d", rc);
    return ESP_FAIL;
}
