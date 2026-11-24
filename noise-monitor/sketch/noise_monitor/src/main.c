#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#if CONFIG_PM_ENABLE && APP_ENABLE_PM
#include "esp_pm.h"
#endif

#include "alert.h"
#include "app_config.h"
#include "app_types.h"
#include "display.h"
#include "freertos_app.h"
#include "sensor.h"
#include "uart.h"
#include "web_dashboard.h"

static const char *TAG = "main";
static app_context_t s_ctx;

static void init_power_management(void)
{
#if CONFIG_PM_ENABLE && APP_ENABLE_PM
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };
    esp_err_t ret = esp_pm_configure(&pm_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Power management active (DFS %d..%d MHz, light_sleep=%s)",
                 pm_cfg.min_freq_mhz, pm_cfg.max_freq_mhz,
                 pm_cfg.light_sleep_enable ? "on" : "off");
    } else {
        ESP_LOGW(TAG, "esp_pm_configure skipped: %s", esp_err_to_name(ret));
    }
#else
    ESP_LOGI(TAG, "Power management disabled in configuration");
#endif
}

static void init_watchdog(void)
{
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = APP_WDT_TIMEOUT_MS,
        .idle_core_mask = (1U << 0) | (1U << 1),
        .trigger_panic = true,
    };

    esp_err_t ret = esp_task_wdt_reconfigure(&twdt_cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = esp_task_wdt_init(&twdt_cfg);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Task Watchdog configured (timeout=%d ms, panic=on)", APP_WDT_TIMEOUT_MS);
    } else {
        ESP_LOGW(TAG, "Task Watchdog setup warning: %s", esp_err_to_name(ret));
    }
}

static void init_default_runtime_config(app_runtime_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->is_calibrated = APP_CALIBRATED_MODE_DEFAULT;
    cfg->cal_ref_db = APP_CAL_REF_DB;
    cfg->cal_ref_rms = APP_CAL_REF_RMS;
    cfg->rel_full_scale_rms = APP_REL_FULL_SCALE_RMS;
    cfg->ema_alpha = APP_EMA_ALPHA;
    cfg->threshold_trigger = APP_THRESHOLD_TRIGGER_DEFAULT;
    cfg->threshold_reset = APP_THRESHOLD_RESET_DEFAULT;
    cfg->preset_index = 0;
    cfg->muted = false;
}

void app_init(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, " Real-Time Noise Pollution Monitoring System");
    ESP_LOGI(TAG, " Target: ESP32-S3 (%d cores) | OS: FreeRTOS", chip_info.cores);
    ESP_LOGI(TAG, "====================================================");

    /* 1. Initialize NVS flash (required for calibration/Wi-Fi stack) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Configure Power Management and Task Watchdog Timer */
    init_power_management();
    init_watchdog();

    /* 3. Initialize default runtime configuration */
    init_default_runtime_config(&s_ctx.config);

    /* 4. Initialize FreeRTOS IPC primitives (queues, semaphores, mutexes) */
    ESP_ERROR_CHECK(freertos_init_ipc(&s_ctx));

    /* 5. Initialize peripherals */
    ESP_ERROR_CHECK(uart_init());
    ESP_ERROR_CHECK(alert_init(&s_ctx));

    ret = display_init(&s_ctx);
    if (ret == ESP_OK) {
        display_show_splash(&s_ctx);
    }

    ESP_ERROR_CHECK(sensor_init());

#if APP_WEB_ENABLE
    ret = web_dashboard_start(&s_ctx);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Web dashboard failed to start (%s); continuing offline", esp_err_to_name(ret));
    }
#endif

    /* 6. Start multitasking via FreeRTOS scheduler */
    ESP_ERROR_CHECK(freertos_tasks_start(&s_ctx));

    ESP_LOGI(TAG, "System running. Mode: %s | Trigger: %.1f | Reset: %.1f",
             s_ctx.config.is_calibrated ? "CALIBRATED dB SPL" : "RELATIVE (%)",
             (double)s_ctx.config.threshold_trigger,
             (double)s_ctx.config.threshold_reset);
}

#ifndef ARDUINO
void app_main(void)
{
    app_init();
}
#endif
