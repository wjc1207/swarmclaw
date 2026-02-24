#pragma once

#include "esp_err.h"

/**
 * Initialize SNTP and block until time is synchronized (or timeout).
 * Sets timezone from MIMI_TIMEZONE on success.
 */
esp_err_t time_sync_init(void);

/**
 * Start the periodic resync timer to compensate for RTC drift.
 * Should be called after time_sync_init(). The timer fires periodically
 * regardless of whether the initial sync succeeded, allowing recovery
 * from a failed boot-time sync.
 */
esp_err_t time_sync_start(void);

/**
 * Stop the periodic resync timer.
 */
void time_sync_stop(void);
