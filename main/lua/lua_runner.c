#include "lua/lua_runner.h"
#include "lua/lua_gpio_lib.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const char *TAG = "lua_runner";

/* ── Capture buffer for print() output ────────────────────── */

#define CAPTURE_BUF_MAX  4096
#define LUA_TASK_STACK   16384

typedef struct {
    char  buf[CAPTURE_BUF_MAX];
    size_t len;
} capture_ctx_t;

/* ── PSRAM allocator for Lua state ────────────────────────── */

static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ── Replacement print() that writes to the capture buffer ── */

static int l_capture_print(lua_State *L)
{
    capture_ctx_t *ctx;
    lua_getfield(L, LUA_REGISTRYINDEX, "_capture_ctx");
    ctx = (capture_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!ctx) return 0;

    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1 && ctx->len < CAPTURE_BUF_MAX - 1) {
            ctx->buf[ctx->len++] = '\t';
        }
        size_t slen;
        const char *s = luaL_tolstring(L, i, &slen);
        if (s) {
            size_t avail = CAPTURE_BUF_MAX - 1 - ctx->len;
            size_t copy = (slen < avail) ? slen : avail;
            memcpy(ctx->buf + ctx->len, s, copy);
            ctx->len += copy;
        }
        lua_pop(L, 1); /* pop the tostring result */
    }
    if (ctx->len < CAPTURE_BUF_MAX - 1) {
        ctx->buf[ctx->len++] = '\n';
    }
    return 0;
}

/* ── Task wrapper for executing Lua with a timeout ────────── */

typedef struct {
    lua_State        *L;
    const char       *script_path;
    int               result;
    SemaphoreHandle_t done_sem;
} lua_task_ctx_t;

static void lua_exec_task(void *arg)
{
    lua_task_ctx_t *tc = (lua_task_ctx_t *)arg;
    tc->result = luaL_dofile(tc->L, tc->script_path);
    xSemaphoreGive(tc->done_sem);
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────── */

esp_err_t lua_runner_exec(const char *script_path, int timeout_ms,
                          char **out_buf)
{
    if (!script_path || !out_buf) return ESP_ERR_INVALID_ARG;

    /* Create a fresh Lua state with PSRAM allocator */
    lua_State *L = lua_newstate(lua_psram_alloc, NULL);
    if (!L) {
        *out_buf = strdup("Failed to create Lua state (out of memory)");
        return ESP_FAIL;
    }

    luaL_openlibs(L);
    lua_open_gpio_libs(L);

    /* Set up capture context */
    capture_ctx_t ctx = { .len = 0 };
    ctx.buf[0] = '\0';
    lua_pushlightuserdata(L, &ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "_capture_ctx");

    /* Replace print() */
    lua_pushcfunction(L, l_capture_print);
    lua_setglobal(L, "print");

    /* Set Lua package search path to SPIFFS scripts directory */
    if (luaL_dostring(L, "package.path = '/spiffs/scripts/?.lua'") != LUA_OK) {
        ESP_LOGW(TAG, "Failed to set package.path: %s",
                 lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    /* Run the script in a separate FreeRTOS task with timeout */
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        lua_close(L);
        *out_buf = strdup("Failed to create semaphore");
        return ESP_FAIL;
    }

    lua_task_ctx_t tc = {
        .L = L,
        .script_path = script_path,
        .result = LUA_ERRRUN,
        .done_sem = done_sem,
    };

    TaskHandle_t task_handle = NULL;
    BaseType_t created = xTaskCreatePinnedToCore(
        lua_exec_task, "lua_exec", LUA_TASK_STACK, &tc,
        tskIDLE_PRIORITY + 1, &task_handle, tskNO_AFFINITY);

    if (created != pdPASS) {
        vSemaphoreDelete(done_sem);
        lua_close(L);
        *out_buf = strdup("Failed to create Lua execution task");
        return ESP_FAIL;
    }

    /* Wait for the task to signal completion or timeout */
    bool timed_out = (xSemaphoreTake(done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE);
    if (timed_out) {
        ESP_LOGW(TAG, "Lua script timed out after %d ms", timeout_ms);
        vTaskDelete(task_handle);
    }
    vSemaphoreDelete(done_sem);

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
        lua_close(L);
        return ESP_FAIL;
    }

    if (tc.result == LUA_OK) {
        *out_buf = strdup(ctx.buf);
    } else {
        const char *err = lua_tostring(L, -1);
        const char *err_msg = err ? err : "unknown error";
        size_t err_len = strlen(err_msg);
        size_t needed = ctx.len + err_len + 2;
        char *result = malloc(needed);
        if (result) {
            if (ctx.len > 0) {
                memcpy(result, ctx.buf, ctx.len);
                result[ctx.len] = '\n';
                memcpy(result + ctx.len + 1, err_msg, err_len + 1);
            } else {
                memcpy(result, err_msg, err_len + 1);
            }
        }
        *out_buf = result ? result : strdup("Lua error");
    }

    lua_close(L);
    ESP_LOGI(TAG, "Script %s finished (rc=%d)", script_path, tc.result);
    return (tc.result == LUA_OK) ? ESP_OK : ESP_FAIL;
}
