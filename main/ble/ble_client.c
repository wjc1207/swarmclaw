#include "ble/ble_client.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_client";

#define DEFAULT_CONNECT_TIMEOUT_MS 15000
#define DEFAULT_READ_TIMEOUT_MS     5000

#define EVT_SYNCED          BIT0
#define EVT_CONNECT_FAILED  BIT1
#define EVT_DISCOVERY_READY BIT2
#define EVT_DISCONNECTED    BIT3
#define EVT_TEMP_DONE       BIT4
#define EVT_HUM_DONE        BIT5
#define EVT_READ_FAILED     BIT6

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static bool s_started;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_connect_pending;
static bool s_chars_ready;

static uint16_t s_ess_start_handle;
static uint16_t s_ess_end_handle;
static uint16_t s_temp_val_handle;
static uint16_t s_hum_val_handle;
static ble_addr_t s_target_addr_raw;
static char s_target_addr[18];

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

static void ble_reset_discovery_state(void)
{
    s_chars_ready = false;
    s_ess_start_handle = 0;
    s_ess_end_handle = 0;
    s_temp_val_handle = 0;
    s_hum_val_handle = 0;
    s_last_measurement.temperature_valid = false;
    s_last_measurement.humidity_valid = false;
}

static int ble_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;

    EventBits_t done_bit = (EventBits_t)(uintptr_t)arg;
    if (error->status != 0 || attr == NULL) {
        ESP_LOGW(TAG, "GATT read failed status=%d handle=0x%04x",
                 error->status, attr ? attr->handle : 0);
        xEventGroupSetBits(s_events, EVT_READ_FAILED);
        return 0;
    }

    uint8_t buf[8] = {0};
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &out_len);
    if (rc != 0 || out_len < 2) {
        ESP_LOGW(TAG, "GATT value parse failed rc=%d len=%u", rc, out_len);
        xEventGroupSetBits(s_events, EVT_READ_FAILED);
        return 0;
    }

    if (done_bit == EVT_TEMP_DONE) {
        int16_t raw_temp = (int16_t)((buf[1] << 8) | buf[0]);
        s_last_measurement.temperature_c = (float)raw_temp / 100.0f;
        s_last_measurement.temperature_valid = true;
    } else if (done_bit == EVT_HUM_DONE) {
        uint16_t raw_hum = (uint16_t)((buf[1] << 8) | buf[0]);
        s_last_measurement.humidity_percent = (float)raw_hum / 100.0f;
        s_last_measurement.humidity_valid = true;
    }

    xEventGroupSetBits(s_events, done_bit);
    return 0;
}

static int ble_disc_hum_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        s_hum_val_handle = chr->val_handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_temp_val_handle != 0 && s_hum_val_handle != 0) {
            s_chars_ready = true;
            xEventGroupSetBits(s_events, EVT_DISCOVERY_READY);
            ESP_LOGI(TAG, "ESS ready temp=0x%04x hum=0x%04x", s_temp_val_handle, s_hum_val_handle);
        } else {
            xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
            ESP_LOGE(TAG, "Humidity characteristic missing");
        }
    } else {
        xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
        ESP_LOGE(TAG, "Humidity characteristic discovery error=%d", error->status);
    }
    return 0;
}

static int ble_disc_temp_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        s_temp_val_handle = chr->val_handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        ble_uuid16_t hum_uuid = BLE_UUID16_INIT(0x2A6F);
        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_ess_start_handle, s_ess_end_handle,
                                             &hum_uuid.u, ble_disc_hum_chr_cb, NULL);
        if (rc != 0) {
            xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
            ESP_LOGE(TAG, "Humidity characteristic discovery start failed rc=%d", rc);
        }
    } else {
        xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
        ESP_LOGE(TAG, "Temperature characteristic discovery error=%d", error->status);
    }
    return 0;
}

static int ble_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;

    if (error->status == 0 && service != NULL) {
        s_ess_start_handle = service->start_handle;
        s_ess_end_handle = service->end_handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (s_ess_start_handle == 0 || s_ess_end_handle == 0) {
            xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
            ESP_LOGE(TAG, "Environmental Sensing service not found");
            return 0;
        }

        ble_uuid16_t temp_uuid = BLE_UUID16_INIT(0x2A6E);
        int rc = ble_gattc_disc_chrs_by_uuid(conn_handle, s_ess_start_handle, s_ess_end_handle,
                                             &temp_uuid.u, ble_disc_temp_chr_cb, NULL);
        if (rc != 0) {
            xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
            ESP_LOGE(TAG, "Temperature characteristic discovery start failed rc=%d", rc);
        }
    } else {
        xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
        ESP_LOGE(TAG, "Service discovery error=%d", error->status);
    }
    return 0;
}

static void ble_try_connect_target(void)
{
    if (!s_connect_pending || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    int rc = ble_gap_connect(s_own_addr_type, &s_target_addr_raw, 30000, NULL, ble_gap_event, NULL);
    if (rc == 0) {
        s_connect_pending = false;
        ESP_LOGI(TAG, "Connecting to %s", s_target_addr);
        return;
    }

    if (rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Connect deferred rc=%d", rc);
        return;
    }

    s_connect_pending = false;
    xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
    ESP_LOGE(TAG, "ble_gap_connect failed rc=%d", rc);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char addr_str[18] = {0};
        ble_addr_to_str(&event->disc.addr, addr_str, sizeof(addr_str));
        if (strcasecmp(addr_str, s_target_addr) == 0) {
            s_target_addr_raw = event->disc.addr;
            s_connect_pending = true;
            ESP_LOGI(TAG, "Found target %s (type=%d)", addr_str, event->disc.addr.type);
            ble_gap_disc_cancel();
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_connect_pending) {
            ble_try_connect_target();
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ble_uuid16_t svc_uuid = BLE_UUID16_INIT(0x181A);
            s_conn_handle = event->connect.conn_handle;
            ble_reset_discovery_state();
            ESP_LOGI(TAG, "Connected, conn_handle=%u", s_conn_handle);
            int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &svc_uuid.u, ble_disc_svc_cb, NULL);
            if (rc != 0) {
                xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
                ESP_LOGE(TAG, "Service discovery start failed rc=%d", rc);
            }
        } else {
            xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
            ESP_LOGW(TAG, "Connect failed status=%d", event->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connect_pending = false;
        ble_reset_discovery_state();
        xEventGroupSetBits(s_events, EVT_DISCONNECTED);
        return 0;
    default:
        return 0;
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset reason=%d", reason);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_connect_pending = false;
    ble_reset_discovery_state();
    xEventGroupClearBits(s_events, EVT_SYNCED);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        xEventGroupSetBits(s_events, EVT_CONNECT_FAILED);
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

    esp_err_t ret = esp_nimble_hci_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "esp_nimble_hci_init failed");

    nimble_port_init();
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
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_chars_ready;
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

    if (ble_client_is_connected()) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_target_addr, target_addr, sizeof(s_target_addr));
    s_connect_pending = false;
    ble_reset_discovery_state();
    xEventGroupClearBits(s_events, EVT_CONNECT_FAILED | EVT_DISCOVERY_READY | EVT_DISCONNECTED |
                                   EVT_TEMP_DONE | EVT_HUM_DONE | EVT_READ_FAILED);

    const struct ble_gap_disc_params scan_params = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &scan_params, ble_gap_event, NULL);
    if (rc != 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_DISCOVERY_READY | EVT_CONNECT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & EVT_DISCOVERY_READY) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    ble_gap_disc_cancel();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    xSemaphoreGive(s_mutex);
    return (bits & EVT_CONNECT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t ble_client_read_measurement(ble_measurement_t *measurement, uint32_t timeout_ms)
{
    if (measurement == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ble_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_READ_TIMEOUT_MS;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_last_measurement.temperature_valid = false;
    s_last_measurement.humidity_valid = false;
    xEventGroupClearBits(s_events, EVT_TEMP_DONE | EVT_HUM_DONE | EVT_READ_FAILED);

    int rc = ble_gattc_read(s_conn_handle, s_temp_val_handle, ble_read_cb, (void *)(uintptr_t)EVT_TEMP_DONE);
    if (rc == 0) {
        rc = ble_gattc_read(s_conn_handle, s_hum_val_handle, ble_read_cb, (void *)(uintptr_t)EVT_HUM_DONE);
    }
    if (rc != 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "ble_gattc_read failed rc=%d", rc);
        return ESP_FAIL;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupGetBits(s_events);
    while ((bits & EVT_READ_FAILED) == 0 &&
           (bits & (EVT_TEMP_DONE | EVT_HUM_DONE)) != (EVT_TEMP_DONE | EVT_HUM_DONE)) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) {
            break;
        }

        TickType_t remaining = deadline - now;
        bits |= xEventGroupWaitBits(s_events, EVT_TEMP_DONE | EVT_HUM_DONE | EVT_READ_FAILED,
                                    pdFALSE, pdFALSE, remaining);
    }

    if (bits & EVT_READ_FAILED) {
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    if ((bits & (EVT_TEMP_DONE | EVT_HUM_DONE)) != (EVT_TEMP_DONE | EVT_HUM_DONE)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_TIMEOUT;
    }

    *measurement = s_last_measurement;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t ble_client_disconnect(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_READ_TIMEOUT_MS;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ble_reset_discovery_state();
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    xEventGroupClearBits(s_events, EVT_DISCONNECTED);
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "ble_gap_terminate failed rc=%d", rc);
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_DISCONNECTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(s_mutex);
    return (bits & EVT_DISCONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
