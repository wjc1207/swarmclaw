#include "tools/tool_script.h"
#include "mpy/mpy_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_script";

#define SCRIPTS_PREFIX "/spiffs/scripts/"

/* ── Helpers ──────────────────────────────────────────────── */

static bool validate_script_path(const char *path)
{
    if (!path) return false;
    if (strncmp(path, SCRIPTS_PREFIX, strlen(SCRIPTS_PREFIX)) != 0) return false;
    if (strstr(path, "..") != NULL) return false;
    /* Must have at least one character after the prefix */
    if (strlen(path) <= strlen(SCRIPTS_PREFIX)) return false;
    return true;
}

/* ── script_write ─────────────────────────────────────────── */

esp_err_t tool_script_write_execute(const char *input_json,
                                    char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid JSON input\"}");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path    = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));

    if (!validate_script_path(path)) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"path must start with %s and must not contain '..'\"}",
                 SCRIPTS_PREFIX);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!content) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"missing 'content' field\"}");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"cannot open file for writing: %s\"}", path);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    if (written != len) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"wrote %d of %d bytes\"}", (int)written, (int)len);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    snprintf(output, output_size, "{\"ok\":true}");
    ESP_LOGI(TAG, "script_write: %s (%d bytes)", path, (int)written);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── script_run ───────────────────────────────────────────── */

esp_err_t tool_script_run_execute(const char *input_json,
                                  char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid JSON input\"}");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    int timeout_ms = 5000;
    cJSON *jtimeout = cJSON_GetObjectItem(root, "timeout_ms");
    if (cJSON_IsNumber(jtimeout)) {
        timeout_ms = jtimeout->valueint;
    }

    if (!validate_script_path(path)) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"path must start with %s and must not contain '..'\"}",
                 SCRIPTS_PREFIX);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Make a local copy of path before we free the JSON */
    size_t path_len = strlen(path);
    if (path_len >= 256) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"path too long (max 255 chars)\"}");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    char path_buf[256];
    memcpy(path_buf, path, path_len + 1);
    cJSON_Delete(root);

    char *script_output = NULL;
    esp_err_t err = mpy_runner_exec(path_buf, timeout_ms, &script_output);

    if (err == ESP_OK) {
        /* Escape output for JSON */
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 1);
        cJSON_AddStringToObject(resp, "output", script_output ? script_output : "");
        char *json_str = cJSON_PrintUnformatted(resp);
        if (json_str) {
            snprintf(output, output_size, "%s", json_str);
            free(json_str);
        } else {
            snprintf(output, output_size, "{\"ok\":true,\"output\":\"\"}");
        }
        cJSON_Delete(resp);
    } else {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", 0);
        cJSON_AddStringToObject(resp, "error", script_output ? script_output : "unknown error");
        char *json_str = cJSON_PrintUnformatted(resp);
        if (json_str) {
            snprintf(output, output_size, "%s", json_str);
            free(json_str);
        } else {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"unknown error\"}");
        }
        cJSON_Delete(resp);
    }

    free(script_output);
    ESP_LOGI(TAG, "script_run: %s → %s", path_buf, (err == ESP_OK) ? "ok" : "fail");
    return err;
}
