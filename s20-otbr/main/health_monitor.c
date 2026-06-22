#include "health_monitor.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_config.h"
#include "led.h"

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/thread_ftd.h"
#include "openthread/platform/radio.h"

#include "esp_rcp_update.h"

#include <string.h>

#define TAG "health_mon"

#define HEALTH_CHECK_INTERVAL_S       30
#define HEALTH_LOCK_TIMEOUT_TICKS     pdMS_TO_TICKS(5000)
#define HEALTH_DETACHED_RCP_RESET_S   1800
#define HEALTH_DETACHED_RESTART_S     3600
#define HEALTH_HEAP_WARN_KB           20
#define HEALTH_HEAP_CRIT_KB           10
#define HEALTH_CRASH_COUNT_MAX        5
#define HEALTH_CRASH_WINDOW_S         600
#define HEALTH_SAFE_MODE_DELAY_S      300
#define HEALTH_STARTUP_GRACE_S        60
#define HEALTH_TASK_STACK             4096
#define HEALTH_TASK_PRIORITY          4

#define NVS_KEY_CRASH_COUNT   "hm_crash"
#define NVS_KEY_CRASH_TIME    "hm_crash_t"

static TaskHandle_t s_health_task = NULL;
static esp_timer_handle_t s_health_timer = NULL;
static bool s_was_ever_attached = false;
static TickType_t s_detached_since = 0;
static int s_rcp_reset_count = 0;
static bool s_safe_mode = false;
static bool s_ot_start_allowed = true;
static bool s_shutdown_registered = false;

static const char *reset_reason_to_string(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT_PIN";
    case ESP_RST_SW:         return "SOFTWARE";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "UNKNOWN";
    }
}

static bool reset_reason_is_crash(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_CPU_LOCKUP;
}

static void shutdown_log_handler(void)
{
    ESP_LOGI(TAG, "Shutdown: uptime=%llus, safe_mode=%d",
             (unsigned long long)(esp_timer_get_time() / 1000000), s_safe_mode);
}

static void health_timer_callback(void *arg)
{
    (void)arg;
    if (s_health_task) {
        xTaskNotifyGive(s_health_task);
    }
}

static void check_heap(void)
{
    uint32_t free_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
    if (free_kb < HEALTH_HEAP_CRIT_KB) {
        ESP_LOGE(TAG, "Critical heap: %luKB < %dKB, restarting",
                 (unsigned long)free_kb, HEALTH_HEAP_CRIT_KB);
        set_pwr_led_color(true, false, false);
        esp_restart();
    }
    if (free_kb < HEALTH_HEAP_WARN_KB) {
        ESP_LOGW(TAG, "Low heap: %luKB < %dKB", (unsigned long)free_kb, HEALTH_HEAP_WARN_KB);
    }
}

static void check_ot_liveness(void)
{
    if (!esp_openthread_lock_acquire(HEALTH_LOCK_TIMEOUT_TICKS)) {
        ESP_LOGE(TAG, "OpenThread lock stuck for %dms, restarting",
                 (int)pdTICKS_TO_MS(HEALTH_LOCK_TIMEOUT_TICKS));
        set_pwr_led_color(true, false, false);
        esp_restart();
    }

    otInstance *ins = esp_openthread_get_instance();
    if (!ins) {
        esp_openthread_lock_release();
        ESP_LOGW(TAG, "OpenThread instance not available yet");
        return;
    }

    otDeviceRole role = otThreadGetDeviceRole(ins);
    esp_openthread_lock_release();

    if (role >= OT_DEVICE_ROLE_CHILD) {
        if (!s_was_ever_attached) {
            ESP_LOGI(TAG, "Thread attached (role=%d), monitoring active", role);
            s_was_ever_attached = true;
        }
        s_detached_since = 0;
        s_rcp_reset_count = 0;
        set_pwr_led_color(false, true, false);
    } else if (role == OT_DEVICE_ROLE_DETACHED || role == OT_DEVICE_ROLE_DISABLED) {
        if (s_was_ever_attached) {
            if (s_detached_since == 0) {
                s_detached_since = xTaskGetTickCount();
                ESP_LOGW(TAG, "Thread detached (role=%d), starting timeout watch", role);
            }

            uint32_t detached_s = (uint32_t)(pdTICKS_TO_MS(xTaskGetTickCount() - s_detached_since) / 1000);

            if (detached_s >= HEALTH_DETACHED_RESTART_S && s_rcp_reset_count >= 1) {
                ESP_LOGE(TAG, "Thread detached for %lus after RCP reset, restarting host",
                         detached_s);
                set_pwr_led_color(true, false, false);
                esp_restart();
            } else if (detached_s >= HEALTH_DETACHED_RCP_RESET_S && s_rcp_reset_count == 0) {
                ESP_LOGW(TAG, "Thread detached for %lus, resetting RCP", detached_s);
                set_pwr_led_color(true, false, false);
                esp_rcp_reset();
                s_rcp_reset_count++;
                s_detached_since = xTaskGetTickCount();
                set_pwr_led_color(false, true, false);
            }
        }
    }
}

static void health_monitor_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    for (;;) {
        xTaskNotifyWait(0, 0xFFFFFFFF, NULL, portMAX_DELAY);

        esp_task_wdt_reset();

        int64_t uptime_s = esp_timer_get_time() / 1000000;
        if (uptime_s < HEALTH_STARTUP_GRACE_S) {
            continue;
        }

        check_heap();
        check_ot_liveness();
    }
}

static void safe_mode_wait_and_recover(void)
{
    ESP_LOGW(TAG, "Safe mode: waiting %ds before Thread start", HEALTH_SAFE_MODE_DELAY_S);
    set_pwr_led_color(true, false, false);

    for (int i = 0; i < HEALTH_SAFE_MODE_DELAY_S; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i > 0 && i % 30 == 0) {
            ESP_LOGI(TAG, "Safe mode: %d/%ds elapsed, free_heap=%luKB",
                     i, HEALTH_SAFE_MODE_DELAY_S,
                     (unsigned long)(esp_get_free_heap_size() / 1024));
        }
    }

    char buf[16];
    if (nvs_config_set(NVS_KEY_CRASH_COUNT, "0") == ESP_OK) {
        ESP_LOGI(TAG, "Safe mode: crash count reset, rebooting to normal operation");
    }
    snprintf(buf, sizeof(buf), "%lld", (long long)0);
    nvs_config_set(NVS_KEY_CRASH_TIME, buf);

    esp_restart();
}

esp_err_t health_monitor_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    bool is_crash = reset_reason_is_crash(reason);
    uint8_t crash_count = 0;
    char crash_str[16];
    char crash_time_str[16];

    ESP_LOGI(TAG, "Boot reset reason: %s", reset_reason_to_string(reason));

    if (nvs_config_get(NVS_KEY_CRASH_COUNT, crash_str, sizeof(crash_str)) == ESP_OK) {
        crash_count = (uint8_t)atoi(crash_str);
    }

    if (is_crash) {
        crash_count++;
        snprintf(crash_str, sizeof(crash_str), "%u", crash_count);
        nvs_config_set(NVS_KEY_CRASH_COUNT, crash_str);

        snprintf(crash_time_str, sizeof(crash_time_str), "%lld",
                 (long long)(esp_timer_get_time() / 1000000));
        nvs_config_set(NVS_KEY_CRASH_TIME, crash_time_str);

        ESP_LOGW(TAG, "Previous crash detected, crash count=%u/%d",
                 crash_count, HEALTH_CRASH_COUNT_MAX);
    }

    if (crash_count >= HEALTH_CRASH_COUNT_MAX) {
        int64_t last_crash_time = 0;
        if (nvs_config_get(NVS_KEY_CRASH_TIME, crash_time_str, sizeof(crash_time_str)) == ESP_OK) {
            last_crash_time = strtoll(crash_time_str, NULL, 10);
        }

        int64_t now_s = esp_timer_get_time() / 1000000;
        if (last_crash_time == 0 || (now_s - last_crash_time) < HEALTH_CRASH_WINDOW_S) {
            ESP_LOGE(TAG, "Boot-loop detected: %u crashes within window, entering safe mode",
                     crash_count);
            s_safe_mode = true;
            s_ot_start_allowed = false;
        } else {
            ESP_LOGI(TAG, "Crash count high but outside window, resetting counter");
            nvs_config_set(NVS_KEY_CRASH_COUNT, "0");
            crash_count = 0;
        }
    }

    if (!s_shutdown_registered) {
        esp_register_shutdown_handler(shutdown_log_handler);
        s_shutdown_registered = true;
    }

    if (s_safe_mode) {
        safe_mode_wait_and_recover();
    }

    return ESP_OK;
}

bool health_monitor_should_start_thread(void)
{
    return s_ot_start_allowed;
}

esp_err_t health_monitor_start(void)
{
    if (s_health_task) {
        return ESP_OK;
    }

    if (s_safe_mode) {
        ESP_LOGW(TAG, "In safe mode, health monitor task not started");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(health_monitor_task, "health_mon",
                                 HEALTH_TASK_STACK, NULL,
                                 HEALTH_TASK_PRIORITY, &s_health_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create health monitor task");
        return ESP_ERR_NO_MEM;
    }

    esp_timer_create_args_t timer_cfg = {
        .callback = health_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "health_mon_tick",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&timer_cfg, &s_health_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create health timer: %s", esp_err_to_name(err));
        vTaskDelete(s_health_task);
        s_health_task = NULL;
        return err;
    }

    err = esp_timer_start_periodic(s_health_timer,
                                   HEALTH_CHECK_INTERVAL_S * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start health timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_health_timer);
        s_health_timer = NULL;
        vTaskDelete(s_health_task);
        s_health_task = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Health monitor started (interval=%ds, detached_timeout=%dmin)",
             HEALTH_CHECK_INTERVAL_S, HEALTH_DETACHED_RCP_RESET_S / 60);

    return ESP_OK;
}
