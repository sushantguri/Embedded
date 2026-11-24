#include "freertos_app.h"

#ifndef HOST_TEST
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "alert.h"
#include "app_config.h"
#include "display.h"
#include "processing.h"
#include "sensor.h"
#include "uart.h"
#include "web_dashboard.h"

static const char *TAG = "freertos";

esp_err_t freertos_init_ipc(app_context_t *ctx)
{
    ctx->raw_audio_queue = xQueueCreate(APP_RAW_AUDIO_QUEUE_LEN, sizeof(audio_block_t));
    ctx->display_queue = xQueueCreate(1, sizeof(noise_measurement_t));
    ctx->alert_queue = xQueueCreate(1, sizeof(noise_measurement_t));
    ctx->logger_queue = xQueueCreate(APP_LOGGER_QUEUE_LEN, sizeof(noise_measurement_t));
    ctx->button_sem = xSemaphoreCreateBinary();
    ctx->state_mutex = xSemaphoreCreateMutex();
    ctx->i2c_mutex = xSemaphoreCreateMutex();

    if (ctx->raw_audio_queue == NULL || ctx->display_queue == NULL ||
        ctx->alert_queue == NULL || ctx->logger_queue == NULL ||
        ctx->button_sem == NULL || ctx->state_mutex == NULL || ctx->i2c_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* =========================================================================
 * 1. Audio Sampling Task (Core 1)
 * ========================================================================= */
void audio_sampling_task(void *pvParameters)
{
    app_context_t *ctx = (app_context_t *)pvParameters;
    static audio_block_t block;
    uint32_t seq = 0;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Audio Sampling Task started on Core %d", xPortGetCoreID());

    while (1) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        block.capture_timestamp_us = esp_timer_get_time();
        block.seq_num = ++seq;

        esp_err_t ret = sensor_read_block(&block, 100);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Sensor read timeout/error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (xQueueSend(ctx->raw_audio_queue, &block, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                ctx->config.dropped_audio_blocks++;
                xSemaphoreGive(ctx->state_mutex);
            }
        }
    }
}

/* =========================================================================
 * 2. Noise Processing Task (Core 1)
 * ========================================================================= */
void noise_processing_task(void *pvParameters)
{
    app_context_t *ctx = (app_context_t *)pvParameters;
    static audio_block_t block;
    processing_state_t proc_state;
    processing_state_init(&proc_state);

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Noise Processing Task started on Core %d", xPortGetCoreID());

    while (1) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        if (xQueueReceive(ctx->raw_audio_queue, &block, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        app_runtime_config_t local_cfg;
        if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            local_cfg = ctx->config;
            xSemaphoreGive(ctx->state_mutex);
        } else {
            continue;
        }

        noise_measurement_t meas;
        processing_block(&proc_state, &block, &local_cfg, esp_timer_get_time(), &meas);

        /* Propagate auto-unmute back to shared config if cleared */
        if (!meas.alert_active && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            ctx->config.muted = false;
            xSemaphoreGive(ctx->state_mutex);
        }

        /* Fan out latest measurement: overwrite for real-time display & alert queues */
        xQueueOverwrite(ctx->display_queue, &meas);
        xQueueOverwrite(ctx->alert_queue, &meas);

#if APP_WEB_ENABLE
        web_dashboard_publish(&meas);
#endif

        if (xQueueSend(ctx->logger_queue, &meas, 0) != pdTRUE) {
            if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                ctx->config.dropped_log_packets++;
                xSemaphoreGive(ctx->state_mutex);
            }
        }
    }
}

/* =========================================================================
 * 3. Alert Task (Core 1)
 * ========================================================================= */
void alert_task(void *pvParameters)
{
    app_context_t *ctx = (app_context_t *)pvParameters;
    noise_measurement_t latest = {
        .status = NOISE_STATUS_NORMAL,
        .alert_active = false,
    };
    noise_status_t prev_status = NOISE_STATUS_NORMAL;
    int64_t last_pulse_toggle_us = 0;
    bool pulse_on = true;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Alert Task started on Core %d", xPortGetCoreID());

    while (1) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        noise_measurement_t incoming;
        if (xQueueReceive(ctx->alert_queue, &incoming, pdMS_TO_TICKS(50)) == pdTRUE) {
            latest = incoming;
        }

        /* Service debounced push button interrupt semaphore */
        if (xSemaphoreTake(ctx->button_sem, 0) == pdTRUE) {
            alert_handle_button_press(ctx, &latest);
            if (latest.alert_active && xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                latest.muted = ctx->config.muted;
                latest.status = latest.muted ? NOISE_STATUS_MUTED : NOISE_STATUS_WARNING;
                xSemaphoreGive(ctx->state_mutex);
            }
        }

        /* Pulse buzzer at 2 Hz (250 ms ON / 250 ms OFF) when in WARNING state */
        int64_t now_us = esp_timer_get_time();
        if (latest.status == NOISE_STATUS_WARNING) {
            if (prev_status != NOISE_STATUS_WARNING) {
                pulse_on = true;
                last_pulse_toggle_us = now_us;
                ESP_LOGW(TAG, "NOISE WARNING TRIGGERED: %.1f (thr=%.1f, response=%.2f ms)",
                         (double)latest.primary_value,
                         (double)latest.threshold_trigger,
                         (double)(now_us - latest.capture_timestamp_us) / 1000.0);
            } else if ((now_us - last_pulse_toggle_us) >= 250000LL) {
                pulse_on = !pulse_on;
                last_pulse_toggle_us = now_us;
            }
        } else {
            if (prev_status == NOISE_STATUS_WARNING || prev_status == NOISE_STATUS_MUTED) {
                if (latest.status == NOISE_STATUS_NORMAL) {
                    ESP_LOGI(TAG, "Noise level returned to NORMAL (%.1f)",
                             (double)latest.primary_value);
                }
            }
            pulse_on = false;
        }

        alert_apply_state(latest.status, pulse_on);
        prev_status = latest.status;
    }
}

/* =========================================================================
 * 4. Display Task (Core 0)
 * ========================================================================= */
void display_task(void *pvParameters)
{
    app_context_t *ctx = (app_context_t *)pvParameters;
    noise_measurement_t meas;
    bool has_data = false;
    static uint8_t fb[OLED_FB_SIZE];

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "Display Task started on Core %d", xPortGetCoreID());

    while (1) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        if (xQueueReceive(ctx->display_queue, &meas, pdMS_TO_TICKS(APP_OLED_REFRESH_MS)) == pdTRUE) {
            has_data = true;
        }

        if (has_data) {
            display_render_measurement(fb, &meas);
            (void)display_update(ctx, fb);
        }
    }
}

/* =========================================================================
 * 5. UART Task (Core 0)
 * ========================================================================= */
void uart_task(void *pvParameters)
{
    app_context_t *ctx = (app_context_t *)pvParameters;
    noise_measurement_t meas;
    char line[192];
    int64_t last_log_us = 0;
    noise_status_t last_logged_status = NOISE_STATUS_NORMAL;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI(TAG, "UART Task started on Core %d", xPortGetCoreID());

    if (APP_LOG_CSV_FORMAT) {
        const char *hdr = "uptime_ms,seq,rms_raw,rms_smooth,relative_pct,est_db,calibrated,threshold_trigger,status,latency_us\n";
        uart_write_line(hdr, strlen(hdr));
    }

    while (1) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        if (xQueueReceive(ctx->logger_queue, &meas, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        bool status_changed = (meas.status != last_logged_status);
        bool interval_elapsed = ((now_us - last_log_us) >= ((int64_t)APP_LOG_INTERVAL_MS * 1000LL));

        if (!status_changed && !interval_elapsed) {
            continue;
        }

        last_log_us = now_us;
        last_logged_status = meas.status;

        int written = uart_format_record(line, sizeof(line), &meas, APP_LOG_CSV_FORMAT != 0);
        if (written > 0) {
            size_t len = (size_t)written;
            if (len >= sizeof(line)) {
                len = sizeof(line) - 1;
            }
            uart_write_line(line, len);
        }
    }
}

/* =========================================================================
 * Task Scheduler Launch Function
 * ========================================================================= */
esp_err_t freertos_tasks_start(app_context_t *ctx)
{
    BaseType_t ok = pdPASS;

    /*
     * Core 1: Real-time sampling, signal processing, and alert control
     */
    ok &= xTaskCreatePinnedToCore(audio_sampling_task, "audio_sample",
                                  APP_TASK_SAMPLING_STACK, ctx,
                                  APP_TASK_SAMPLING_PRIO, NULL, 1);
    ok &= xTaskCreatePinnedToCore(noise_processing_task, "noise_proc",
                                  APP_TASK_PROCESSING_STACK, ctx,
                                  APP_TASK_PROCESSING_PRIO, NULL, 1);
    ok &= xTaskCreatePinnedToCore(alert_task, "alert_ctrl",
                                  APP_TASK_ALERT_STACK, ctx,
                                  APP_TASK_ALERT_PRIO, NULL, 1);

    /*
     * Core 0: I/O and user interface (OLED display and UART streaming)
     */
    ok &= xTaskCreatePinnedToCore(display_task, "oled_disp",
                                  APP_TASK_DISPLAY_STACK, ctx,
                                  APP_TASK_DISPLAY_PRIO, NULL, 0);
    ok &= xTaskCreatePinnedToCore(uart_task, "uart_comm",
                                  APP_TASK_LOGGER_STACK, ctx,
                                  APP_TASK_LOGGER_PRIO, NULL, 0);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create FreeRTOS tasks!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "All 5 FreeRTOS tasks created & scheduled across Cores 0 and 1.");
    return ESP_OK;
}

#endif /* HOST_TEST */
