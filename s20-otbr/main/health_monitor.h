#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the health monitor and evaluate boot-loop status.
 *
 * Must be called after nvs_config_init() and before
 * launch_openthread_border_router().  Reads the reset reason, updates
 * the crash counter in NVS, and decides whether the device should
 * enter safe mode.
 *
 * @return ESP_OK on success.
 */
esp_err_t health_monitor_init(void);

/**
 * @brief Check whether Thread should be started on this boot.
 *
 * Returns false when the device is in safe mode (too many consecutive
 * crashes).  In safe mode the web server may still be reachable but
 * Thread and the RCP are not started; the monitor will wait for a
 * stability period, reset the crash counter, and reboot.
 *
 * @return true if Thread should start, false if in safe mode.
 */
bool health_monitor_should_start_thread(void);

/**
 * @brief Start the periodic health-monitoring task and timer.
 *
 * Must be called after health_monitor_init() and after
 * launch_openthread_border_router() (or the decision to skip it).
 *
 * @return ESP_OK on success.
 */
esp_err_t health_monitor_start(void);

#ifdef __cplusplus
}
#endif
