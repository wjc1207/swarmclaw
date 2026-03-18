#include "tools/tool_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble/ble_client.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "tool_ble";

static esp_err_t render_json(cJSON *root, char *output, size_t output_size, esp_err_t status)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"json encode failed\"}");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", json);
    free(json);
    cJSON_Delete(root);
    return status;
}

esp_err_t tool_ble_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (root == NULL) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid JSON input\"}");
        return ESP_ERR_INVALID_ARG;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    int timeout_ms = 0;
    cJSON *jtimeout = cJSON_GetObjectItem(root, "timeout_ms");
    if (cJSON_IsNumber(jtimeout)) {
        timeout_ms = jtimeout->valueint;
    }

    if (action == NULL) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"missing action\"}");
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(action, "connect") == 0) {
        const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(root, "addr"));
        char addr_buf[18] = {0};
        if (addr == NULL || addr[0] == '\0') {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"connect requires addr\"}");
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(addr_buf, sizeof(addr_buf), "%s", addr);

        esp_err_t err = ble_client_connect(addr_buf, timeout_ms);
        cJSON_Delete(root);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
        cJSON_AddStringToObject(resp, "action", "connect");
        cJSON_AddStringToObject(resp, "addr", addr_buf);
        cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "connected" : "failed");
        if (err != ESP_OK) {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "BLE connect %s -> %s", addr_buf, err == ESP_OK ? "ok" : "fail");
        return render_json(resp, output, output_size, err);
    }

    if (strcmp(action, "read") == 0) {
        ble_raw_data_t records[BLE_RAW_MAX_RECORDS] = {0};
        size_t record_count = 0;
        esp_err_t err = ble_client_read_all_raw(records, BLE_RAW_MAX_RECORDS, &record_count, timeout_ms);
        cJSON_Delete(root);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
        cJSON_AddStringToObject(resp, "action", "read");
        cJSON_AddNumberToObject(resp, "timeout_ms", timeout_ms);
        cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "ok" : "failed");
        if (err == ESP_OK) {
            cJSON *arr = cJSON_CreateArray();
            for (size_t i = 0; i < record_count; i++) {
                cJSON *item = cJSON_CreateObject();
                char svc_uuid[7] = {0};
                char chr_uuid[7] = {0};
                char raw_hex[(BLE_RAW_MAX_VALUE_LEN * 3) + 1];
                size_t pos = 0;

                snprintf(svc_uuid, sizeof(svc_uuid), "0x%04X", records[i].service_uuid);
                snprintf(chr_uuid, sizeof(chr_uuid), "0x%04X", records[i].characteristic_uuid);
                memset(raw_hex, 0, sizeof(raw_hex));
                for (size_t j = 0; j < records[i].raw_len && j < BLE_RAW_MAX_VALUE_LEN; j++) {
                    if (pos + 3 >= sizeof(raw_hex)) {
                        break;
                    }
                    pos += (size_t)snprintf(&raw_hex[pos], sizeof(raw_hex) - pos,
                                            (j == 0) ? "%02X" : "-%02X", records[i].raw[j]);
                }

                cJSON_AddStringToObject(item, "service_uuid", svc_uuid);
                cJSON_AddStringToObject(item, "characteristic_uuid", chr_uuid);
                cJSON_AddNumberToObject(item, "value_handle", records[i].value_handle);
                cJSON_AddNumberToObject(item, "raw_len", records[i].raw_len);
                cJSON_AddStringToObject(item, "raw_hex", raw_hex);
                cJSON_AddItemToArray(arr, item);
            }
            cJSON_AddItemToObject(resp, "services_raw", arr);
            cJSON_AddNumberToObject(resp, "count", (double)record_count);
        } else {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "BLE read -> %s", err == ESP_OK ? "ok" : "fail");
        return render_json(resp, output, output_size, err);
    }

    if (strcmp(action, "disconnect") == 0) {
        esp_err_t err = ble_client_disconnect(timeout_ms);
        cJSON_Delete(root);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
        cJSON_AddStringToObject(resp, "action", "disconnect");
        cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "disconnected" : "failed");
        if (err != ESP_OK) {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "BLE disconnect -> %s", err == ESP_OK ? "ok" : "fail");
        return render_json(resp, output, output_size, err);
    }

    cJSON_Delete(root);
    snprintf(output, output_size, "{\"ok\":false,\"error\":\"unsupported action\"}");
    return ESP_ERR_INVALID_ARG;
}
