#include "time/time_sync.h"
#include "mimi_config.h"

#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "time_sync";

static TimerHandle_t s_resync_timer = NULL;

/* ── Internal helpers ─────────────────────────────────────────── */

static void log_current_time(void)
{
    time_t now;
    struct tm timeinfo;
    char buf[32];

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Time synchronized: %s (%s)", buf, MIMI_TIMEZONE);
}

/* ── Periodic resync timer callback ──────────────────────────── */

static void resync_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI(TAG, "Triggering periodic SNTP resync...");
    esp_sntp_restart();
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t time_sync_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP (server: %s)...", MIMI_SNTP_SERVER);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, MIMI_SNTP_SERVER);
    esp_sntp_init();

    time_t now = 0;
    int retry = 0;
    const int max_retries = MIMI_SNTP_TIMEOUT_MS / 1000;

    while (time(&now) < MIMI_SNTP_SANE_EPOCH && retry < max_retries) {
        retry++;
        ESP_LOGI(TAG, "Waiting for SNTP sync... (%d/%d)", retry, max_retries);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (now >= MIMI_SNTP_SANE_EPOCH) {
        setenv("TZ", MIMI_TIMEZONE, 1);
        tzset();
        log_current_time();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SNTP sync timeout, system time may be invalid");
    return ESP_ERR_TIMEOUT;
}

esp_err_t time_sync_start(void)
{
    if (s_resync_timer) {
        ESP_LOGW(TAG, "Resync timer already running");
        return ESP_OK;
    }

    s_resync_timer = xTimerCreate(
        "sntp_resync",
        pdMS_TO_TICKS(MIMI_SNTP_RESYNC_INTERVAL_MS),
        pdTRUE,   /* auto-reload */
        NULL,
        resync_timer_callback
    );

    if (!s_resync_timer) {
        ESP_LOGE(TAG, "Failed to create resync timer");
        return ESP_FAIL;
    }

    if (xTimerStart(s_resync_timer, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start resync timer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Periodic resync started (every %d min)",
             MIMI_SNTP_RESYNC_INTERVAL_MS / 60000);
    return ESP_OK;
}

void time_sync_stop(void)
{
    if (s_resync_timer) {
        xTimerStop(s_resync_timer, pdMS_TO_TICKS(1000));
        xTimerDelete(s_resync_timer, pdMS_TO_TICKS(1000));
        s_resync_timer = NULL;
        ESP_LOGI(TAG, "Periodic resync stopped");
    }
}
