#include "mpy/mpy_runner.h"
#include "mpy/mpy_gpio_module.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "port/micropython_embed.h"
#include "port/mphalport.h"

static const char *TAG = "mpy_runner";

/* ── Capture buffer for print() output ────────────────────── */

#define CAPTURE_BUF_MAX  4096
#define MPY_TASK_STACK   16384
#define MPY_HEAP_SIZE    (32 * 1024)

typedef struct {
    char   buf[CAPTURE_BUF_MAX];
    size_t len;
} capture_ctx_t;

/* ── Thread-local capture context ─────────────────────────── */

static capture_ctx_t *s_capture_ctx = NULL;

static void capture_hook(const char *str, size_t len)
{
    capture_ctx_t *ctx = s_capture_ctx;
    if (!ctx) return;

    size_t avail = CAPTURE_BUF_MAX - 1 - ctx->len;
    size_t copy = (len < avail) ? len : avail;
    if (copy > 0) {
        memcpy(ctx->buf + ctx->len, str, copy);
        ctx->len += copy;
    }
}

/* ── Task wrapper for executing MicroPython with a timeout ── */

typedef struct {
    const char       *script_src;
    int               result;       /* 0 = success */
    SemaphoreHandle_t done_sem;
    capture_ctx_t    *ctx;
} mpy_task_ctx_t;

static void mpy_exec_task(void *arg)
{
    mpy_task_ctx_t *tc = (mpy_task_ctx_t *)arg;

    /* Allocate GC heap from PSRAM */
    void *heap = heap_caps_malloc(MPY_HEAP_SIZE,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!heap) {
        tc->result = -1;
        xSemaphoreGive(tc->done_sem);
        vTaskDelete(NULL);
        return;
    }

    /* Install capture hook and init context */
    s_capture_ctx = tc->ctx;
    mpy_set_stdout_hook(capture_hook);

    /* Initialise MicroPython */
    int stack_top;
    mp_embed_init(heap, MPY_HEAP_SIZE, &stack_top);

    /* Register hardware modules */
    mpy_register_hw_modules();

    /* Execute the script string */
    mp_embed_exec_str(tc->script_src);

    /* Tear down */
    mp_embed_deinit();
    mpy_set_stdout_hook(NULL);
    s_capture_ctx = NULL;
    heap_caps_free(heap);

    tc->result = 0;
    xSemaphoreGive(tc->done_sem);
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────── */

esp_err_t mpy_runner_exec(const char *script_path, int timeout_ms,
                          char **out_buf)
{
    if (!script_path || !out_buf) return ESP_ERR_INVALID_ARG;

    /* Read the script file into memory */
    FILE *f = fopen(script_path, "r");
    if (!f) {
        *out_buf = strdup("Cannot open script file");
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 64 * 1024) {
        fclose(f);
        *out_buf = strdup("Script file empty or too large (max 64 KB)");
        return ESP_FAIL;
    }
    char *script_src = malloc((size_t)fsize + 1);
    if (!script_src) {
        fclose(f);
        *out_buf = strdup("Out of memory reading script");
        return ESP_FAIL;
    }
    size_t nread = fread(script_src, 1, (size_t)fsize, f);
    fclose(f);
    script_src[nread] = '\0';

    /* Set up capture context */
    capture_ctx_t ctx = { .len = 0 };
    ctx.buf[0] = '\0';

    /* Run the script in a separate FreeRTOS task with timeout */
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        free(script_src);
        *out_buf = strdup("Failed to create semaphore");
        return ESP_FAIL;
    }

    mpy_task_ctx_t tc = {
        .script_src = script_src,
        .result     = -1,
        .done_sem   = done_sem,
        .ctx        = &ctx,
    };

    TaskHandle_t task_handle = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        mpy_exec_task, "mpy_exec", MPY_TASK_STACK, &tc,
        tskIDLE_PRIORITY + 1, &task_handle, tskNO_AFFINITY);

    if (created != pdPASS) {
        vSemaphoreDelete(done_sem);
        free(script_src);
        *out_buf = strdup("Failed to create MicroPython execution task");
        return ESP_FAIL;
    }

    /* Wait for the task to signal completion or timeout */
    bool timed_out = (xSemaphoreTake(done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE);
    if (timed_out) {
        ESP_LOGW(TAG, "MicroPython script timed out after %d ms", timeout_ms);
        vTaskDelete(task_handle);
        mpy_set_stdout_hook(NULL);
        s_capture_ctx = NULL;
    }
    vSemaphoreDelete(done_sem);
    free(script_src);

    /* Flush capture buffer */
    ctx.buf[ctx.len] = '\0';

    if (timed_out) {
        size_t needed = ctx.len + 64;
        char *result = malloc(needed);
        if (result) {
            snprintf(result, needed, "%s\n[Timeout: script exceeded %d ms]",
                     ctx.buf, timeout_ms);
        }
        *out_buf = result ? result : strdup("[Timeout]");
        return ESP_FAIL;
    }

    /* mp_embed_exec_str always returns void; errors are printed to capture buf */
    if (ctx.len > 0 && strstr(ctx.buf, "Traceback") != NULL) {
        *out_buf = strdup(ctx.buf);
        ESP_LOGI(TAG, "Script %s finished with error", script_path);
        return ESP_FAIL;
    }

    *out_buf = strdup(ctx.buf);
    ESP_LOGI(TAG, "Script %s finished (ok)", script_path);
    return ESP_OK;
}
