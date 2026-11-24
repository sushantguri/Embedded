#ifndef SENSOR_H
#define SENSOR_H

#include <stdbool.h>
#include <stdint.h>
#include "app_types.h"

#ifndef HOST_TEST
#include "esp_err.h"

/**
 * @brief Initialize the configured microphone sensor peripheral (ADC Continuous DMA or I2S).
 */
esp_err_t sensor_init(void);

/**
 * @brief Read one block of audio samples from the microphone sensor.
 *
 * @param[out] out_block  Destination audio block structure.
 * @param[in]  timeout_ms Maximum wait time in milliseconds.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on timeout, or driver error.
 */
esp_err_t sensor_read_block(audio_block_t *out_block, uint32_t timeout_ms);

/**
 * @brief Stop and release microphone sensor hardware resources.
 */
void sensor_deinit(void);

/* Backward compatibility aliases */
static inline esp_err_t microphone_init(void) { return sensor_init(); }
static inline esp_err_t microphone_read_block(audio_block_t *b, uint32_t t) { return sensor_read_block(b, t); }
static inline void microphone_deinit(void) { sensor_deinit(); }
#endif

#endif /* SENSOR_H */
