#ifndef FREERTOS_APP_H
#define FREERTOS_APP_H

#include "app_types.h"

#ifndef HOST_TEST
#include "esp_err.h"

/**
 * @brief Initialize all FreeRTOS queues, semaphores, and mutexes.
 */
esp_err_t freertos_init_ipc(app_context_t *ctx);

/**
 * @brief Create and start all FreeRTOS tasks pinned to appropriate ESP32-S3 CPU cores:
 *        - Core 1: audio_sampling_task, noise_processing_task, alert_task
 *        - Core 0: display_task, uart_task
 */
esp_err_t freertos_tasks_start(app_context_t *ctx);

/* Individual task functions */
void audio_sampling_task(void *pvParameters);
void noise_processing_task(void *pvParameters);
void alert_task(void *pvParameters);
void display_task(void *pvParameters);
void uart_task(void *pvParameters);

/* Alias for logger_task */
static inline void logger_task(void *pvParameters) { uart_task(pvParameters); }
#endif

#endif /* FREERTOS_APP_H */
