// main/mpy/mpy_runner.c

#include "mpy/mpy_runner.h"
#include "micropython_embed.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "mpy_runner";

#define MPY_HEAP_SIZE    (64 * 1024)   // 64 KB from PSRAM
#define MPY_OUTPUT_SIZE  (4  * 1024)   // 4 KB output capture buffer
#define MPY_TASK_STACK   (16 * 1024)   // 16 KB task stack

// Output capture buffer — filled by mp_hal_stdout_tx_strn()
static char  s_output_buf[MPY_OUTPUT_SIZE];
static int   s_output_len = 0;

// MicroPython calls this for ALL print() output — override to capture it
void mp_hal_stdout_tx_strn(const char *str, size_t len) {
    int avail = MPY_OUTPUT_SIZE - s_output_len - 1;
    if (avail <= 0) return;
    int copy = (int)len < avail ? (int)len : avail;
    memcpy(s_output_buf + s_output_len, str, copy);
    s_output_len += copy;
}

/* ── Task wrapper for executing MicroPython with a timeout ── */

typedef struct {
    void              *heap;
    char              *src;
    SemaphoreHandle_t  done_sem;
} mpy_task_ctx_t;

static void mpy_exec_task(void *arg)
{
    mpy_task_ctx_t *tc = (mpy_task_ctx_t *)arg;

    mp_embed_init(tc->heap, MPY_HEAP_SIZE);
    mp_embed_exec_str(tc->src);
    mp_embed_deinit();

    xSemaphoreGive(tc->done_sem);
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────── */

esp_err_t mpy_runner_exec(const char *script_path, int timeout_ms,
                          char **out_buf)
{
    if (!script_path || !out_buf) return ESP_ERR_INVALID_ARG;

    // Reset output buffer
    s_output_len = 0;
    memset(s_output_buf, 0, sizeof(s_output_buf));

    // Allocate interpreter heap from PSRAM
    void *heap = heap_caps_malloc(MPY_HEAP_SIZE, MALLOC_CAP_SPIRAM);
    if (!heap) {
        *out_buf = strdup("Failed to allocate MicroPython heap (out of PSRAM)");
        return ESP_ERR_NO_MEM;
    }

    // Read script from SPIFFS
    FILE *f = fopen(script_path, "r");
    if (!f) {
        heap_caps_free(heap);
        *out_buf = strdup("Cannot open script file");
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    char *src = heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM);
    if (!src) {
        fclose(f);
        heap_caps_free(heap);
        *out_buf = strdup("Failed to allocate script buffer");
        return ESP_ERR_NO_MEM;
    }
    size_t nread = fread(src, 1, fsize, f);
    src[nread] = '\0';
    fclose(f);

    // Run the script in a separate FreeRTOS task with timeout
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        heap_caps_free(src);
        heap_caps_free(heap);
        *out_buf = strdup("Failed to create semaphore");
        return ESP_FAIL;
    }

    mpy_task_ctx_t tc = {
        .heap     = heap,
        .src      = src,
        .done_sem = done_sem,
    };

    TaskHandle_t task_handle = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        mpy_exec_task, "mpy_exec", MPY_TASK_STACK, &tc,
        tskIDLE_PRIORITY + 1, &task_handle, tskNO_AFFINITY);

    if (created != pdPASS) {
        vSemaphoreDelete(done_sem);
        heap_caps_free(src);
        heap_caps_free(heap);
        *out_buf = strdup("Failed to create MicroPython execution task");
        return ESP_FAIL;
    }

    // Wait for the task to signal completion or timeout
    bool timed_out = (xSemaphoreTake(done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE);
    if (timed_out) {
        ESP_LOGW(TAG, "MicroPython script timed out after %d ms", timeout_ms);
        vTaskDelete(task_handle);
    }
    vSemaphoreDelete(done_sem);

    // Clean up PSRAM allocations
    heap_caps_free(src);
    heap_caps_free(heap);

    // Null-terminate output
    s_output_buf[s_output_len] = '\0';

    if (timed_out) {
        size_t needed = s_output_len + 64;
        char *result = malloc(needed);
        if (result) {
            snprintf(result, needed, "%s\n[Timeout: script exceeded %d ms]",
                     s_output_buf, timeout_ms);
        }
        *out_buf = result ? result : strdup("[Timeout]");
        return ESP_FAIL;
    }

    *out_buf = strdup(s_output_buf);
    ESP_LOGI(TAG, "Script %s finished", script_path);
    return ESP_OK;
}
