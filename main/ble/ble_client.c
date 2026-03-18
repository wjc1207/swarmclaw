#include "ble/ble_client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_client";

#define DEFAULT_CONNECT_TIMEOUT_MS 15000
#define DEFAULT_READ_TIMEOUT_MS     5000

#define EVT_SYNCED          BIT0
#define EVT_LISTEN_FAILED   BIT1
#define EVT_RAW_READY       BIT2

#define BLE_MAX_SERVICES             16

typedef struct {
    uint16_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
} ble_service_range_t;

static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_events;
static bool s_started;
static bool s_listening;
static uint8_t s_own_addr_type;
static char s_target_addr[18];
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static uint16_t s_svc_start_handle = 0;
static uint16_t s_svc_end_handle = 0;
static ble_service_range_t s_services[BLE_MAX_SERVICES];
static size_t s_service_count = 0;

static ble_raw_data_t s_raw_records[BLE_RAW_MAX_RECORDS];
static size_t s_raw_count = 0;
static size_t s_raw_next_read_idx = 0;
static int s_raw_inflight_reads = 0;

static ble_measurement_t s_last_measurement;

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static int ble_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg);
static int ble_disc_all_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg);
static int ble_read_raw_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg);

static bool ble_start_next_raw_read(uint16_t conn_handle)
{
    while (s_raw_next_read_idx < s_raw_count) {
        size_t idx = s_raw_next_read_idx++;
        int rc = ble_gattc_read(conn_handle, s_raw_records[idx].value_handle,
                                ble_read_raw_cb, (void *)(intptr_t)idx);
        if (rc == 0) {
            s_raw_inflight_reads++;
            return true;
        }
        ESP_LOGW(TAG, "Read start failed handle=%u rc=%d", s_raw_records[idx].value_handle, rc);
    }
    return false;
}

static void ble_clear_services(void)
{
    memset(s_services, 0, sizeof(s_services));
    s_service_count = 0;
}

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

static void ble_clear_raw_records(void)
{
    memset(s_raw_records, 0, sizeof(s_raw_records));
    s_raw_count = 0;
    s_raw_next_read_idx = 0;
    s_raw_inflight_reads = 0;
}

static int ble_read_raw_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;

    size_t idx = (size_t)(intptr_t)arg;
    if (idx >= s_raw_count) {
        return 0;
    }

    if (error->status == 0 && attr != NULL && attr->om != NULL) {
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(attr->om, s_raw_records[idx].raw,
                                     BLE_RAW_MAX_VALUE_LEN, &out_len);
        if (rc == 0) {
            s_raw_records[idx].raw_len = out_len;
            s_raw_records[idx].valid = true;
            ESP_LOGI(TAG, "Raw read uuid=0x%04x handle=%u len=%u",
                     s_raw_records[idx].characteristic_uuid,
                     s_raw_records[idx].value_handle,
                     s_raw_records[idx].raw_len);
        }
    } else {
        ESP_LOGW(TAG, "Raw read failed status=%d idx=%u", error->status, (unsigned)idx);
    }

    if (s_raw_inflight_reads > 0) {
        s_raw_inflight_reads--;
    }

    if (!ble_start_next_raw_read(conn_handle) && s_raw_inflight_reads == 0) {
        xEventGroupSetBits(s_events, EVT_RAW_READY);
    }
    return 0;
}

static int ble_disc_all_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg)
{
    size_t svc_idx = (size_t)(intptr_t)arg;
    if (svc_idx >= s_service_count) {
        return 0;
    }

    if (error->status == 0 && chr != NULL) {
        if (s_raw_count < BLE_RAW_MAX_RECORDS) {
            uint16_t chr_uuid = ble_uuid_u16((const ble_uuid_t *)&chr->uuid);
            s_raw_records[s_raw_count].service_uuid = s_services[svc_idx].uuid;
            s_raw_records[s_raw_count].characteristic_uuid = chr_uuid;
            s_raw_records[s_raw_count].value_handle = chr->val_handle;
            s_raw_records[s_raw_count].raw_len = 0;
            s_raw_records[s_raw_count].valid = false;
            s_raw_count++;

            ESP_LOGI(TAG, "Found characteristic uuid=0x%04x val_handle=%u", chr_uuid, chr->val_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if ((svc_idx + 1) < s_service_count) {
            size_t next_idx = svc_idx + 1;
            int rc = ble_gattc_disc_all_chrs(conn_handle,
                                             s_services[next_idx].start_handle,
                                             s_services[next_idx].end_handle,
                                             ble_disc_all_chr_cb,
                                             (void *)(intptr_t)next_idx);
            if (rc != 0) {
                ESP_LOGW(TAG, "Start characteristic discovery for next service failed rc=%d", rc);
                xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
            }
            return 0;
        }

        size_t i;
        ESP_LOGI(TAG, "Characteristic discovery complete, count=%u", (unsigned)s_raw_count);
        for (i = 0; i < s_raw_count; i++) {
            s_raw_records[i].valid = false;
            s_raw_records[i].raw_len = 0;
        }

        s_raw_next_read_idx = 0;
        s_raw_inflight_reads = 0;
        if (!ble_start_next_raw_read(conn_handle)) {
            xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
        }
        return 0;
    }

    ESP_LOGW(TAG, "Characteristic discovery error status=%d", error->status);
    return 0;
}

/* Callback for service discovery (EnvironmentalSensing 0x181A) */
static int ble_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;

    /* Collect all discovered services */
    if (error->status == 0 && service != NULL) {
        if (s_service_count < BLE_MAX_SERVICES) {
            uint16_t svc_uuid = ble_uuid_u16((const ble_uuid_t *)&service->uuid);
            s_services[s_service_count].uuid = svc_uuid;
            s_services[s_service_count].start_handle = service->start_handle;
            s_services[s_service_count].end_handle = service->end_handle;
            s_service_count++;
            ESP_LOGI(TAG, "Found service uuid=0x%04x start=%u end=%u",
                     svc_uuid, service->start_handle, service->end_handle);
        }
        return 0;
    }

    /* Discovery complete: start characteristic discovery from first service */
    if (error->status == BLE_HS_EDONE) {
        if (s_service_count == 0) {
            xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
            return 0;
        }

        s_last_measurement.temperature_valid = false;
        s_last_measurement.humidity_valid = false;
        s_svc_start_handle = s_services[0].start_handle;
        s_svc_end_handle = s_services[0].end_handle;
        ble_clear_raw_records();

        int rc = ble_gattc_disc_all_chrs(conn_handle,
                                         s_services[0].start_handle,
                                         s_services[0].end_handle,
                                         ble_disc_all_chr_cb,
                                         (void *)(intptr_t)0);
        if (rc != 0) {
            ESP_LOGW(TAG, "Start characteristic discovery failed rc=%d", rc);
            xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
        }
        return 0;
    }

    /* Log any other errors but continue */
    if (error->status != 0) {
        ESP_LOGW(TAG, "Service discovery error status=%d", error->status);
    }

    return 0;
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "BLE_GAP_EVENT_CONNECT: Failed to find conn_handle=%d", event->connect.conn_handle);
            xEventGroupSetBits(s_events, EVT_LISTEN_FAILED);
            return 0;
        }

        char addr_str[18] = {0};
        ble_addr_to_str(&desc.peer_ota_addr, addr_str, sizeof(addr_str));
        
        if (strcasecmp(addr_str, s_target_addr) != 0) {
            /* Not our target device, disconnect */
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ESP_LOGI(TAG, "Connected to %s conn_handle=%d", addr_str, event->connect.conn_handle);
        s_conn_handle = event->connect.conn_handle;
        s_last_measurement.temperature_valid = false;
        s_last_measurement.humidity_valid = false;
        s_last_measurement.battery_valid = false;
        s_svc_start_handle = 0;
        s_svc_end_handle = 0;
        ble_clear_services();
        ble_clear_raw_records();

        /* Start full service discovery */
        rc = ble_gattc_disc_all_svcs(s_conn_handle, ble_disc_svc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Service discovery start failed rc=%d", rc);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return 0;

    default:
        return 0;
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE reset reason=%d", reason);
    s_listening = false;
    xEventGroupClearBits(s_events, EVT_SYNCED | EVT_RAW_READY);
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
    s_listening = true;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start_handle = 0;
    s_svc_end_handle = 0;
    ble_clear_services();
    ble_clear_raw_records();
    s_last_measurement.temperature_valid = false;
    s_last_measurement.humidity_valid = false;
    s_last_measurement.battery_valid = false;

    xEventGroupClearBits(s_events, EVT_LISTEN_FAILED | EVT_RAW_READY);

    /* Parse target address */
    ble_addr_t peer_addr = {0};
    int rc = sscanf(target_addr, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
                    &peer_addr.val[5], &peer_addr.val[4], &peer_addr.val[3],
                    &peer_addr.val[2], &peer_addr.val[1], &peer_addr.val[0]);
    if (rc != 6) {
        ESP_LOGE(TAG, "Invalid address format: %s", target_addr);
        s_listening = false;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    peer_addr.type = BLE_ADDR_PUBLIC;

    ESP_LOGI(TAG, "GATT Mode: Initiating BLE connection to %s", target_addr);
    rc = ble_gap_connect(s_own_addr_type, &peer_addr, timeout_ms, NULL, ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        s_listening = false;
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_mutex);

    /* Wait for raw data to arrive */
    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_RAW_READY | EVT_LISTEN_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if ((bits & EVT_RAW_READY) == 0) {
        ESP_LOGE(TAG, "No raw data received, timeout or error");
        (void)ble_client_disconnect(1000);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
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

    /* If we have valid measurements in cache, return them */
    if (s_last_measurement.temperature_valid || s_last_measurement.humidity_valid ||
        s_last_measurement.battery_valid) {
        *measurement = s_last_measurement;
        return ESP_OK;
    }

    /* Otherwise issue raw reads for all discovered characteristics */
    xEventGroupClearBits(s_events, EVT_RAW_READY | EVT_LISTEN_FAILED);
    for (size_t i = 0; i < s_raw_count; i++) {
        s_raw_records[i].valid = false;
        s_raw_records[i].raw_len = 0;
    }

    s_raw_next_read_idx = 0;
    s_raw_inflight_reads = 0;

    if (!ble_start_next_raw_read(s_conn_handle)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Wait for reads to complete */
    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_RAW_READY | EVT_LISTEN_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & EVT_RAW_READY) {
        *measurement = s_last_measurement;
        return ESP_OK;
    }

    return (bits & EVT_LISTEN_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t ble_client_read_all_raw(ble_raw_data_t *out_records, size_t max_records,
                                  size_t *out_count, uint32_t timeout_ms)
{
    if (out_records == NULL || out_count == NULL || max_records == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_listening) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        timeout_ms = DEFAULT_READ_TIMEOUT_MS;
    }

    xEventGroupClearBits(s_events, EVT_RAW_READY | EVT_LISTEN_FAILED);
    for (size_t i = 0; i < s_raw_count; i++) {
        s_raw_records[i].valid = false;
        s_raw_records[i].raw_len = 0;
    }

    s_raw_next_read_idx = 0;
    s_raw_inflight_reads = 0;

    if (!ble_start_next_raw_read(s_conn_handle)) {
        return ESP_ERR_NOT_FOUND;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_RAW_READY | EVT_LISTEN_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & EVT_RAW_READY) == 0) {
        return (bits & EVT_LISTEN_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    size_t copied = 0;
    for (size_t i = 0; i < s_raw_count && copied < max_records; i++) {
        if (!s_raw_records[i].valid) {
            continue;
        }
        out_records[copied] = s_raw_records[i];
        copied++;
    }
    *out_count = copied;

    return copied > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
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
    s_target_addr[0] = '\0';
    s_last_measurement.temperature_valid = false;
    s_last_measurement.humidity_valid = false;
    s_last_measurement.battery_valid = false;
    xEventGroupClearBits(s_events, EVT_RAW_READY | EVT_LISTEN_FAILED);

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_EBUSY) {
            ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
        }
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    s_svc_start_handle = 0;
    s_svc_end_handle = 0;
    ble_clear_services();
    ble_clear_raw_records();

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
