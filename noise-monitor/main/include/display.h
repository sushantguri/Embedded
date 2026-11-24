#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_PAGES  (OLED_HEIGHT / 8)
#define OLED_FB_SIZE (OLED_WIDTH * OLED_PAGES)

/**
 * @brief Render a noise measurement into an SSD1306 128x64 monochrome framebuffer.
 *
 * Separated from I2C transport so the UI layout can also be verified in host tests.
 */
void display_render_measurement(uint8_t fb[OLED_FB_SIZE],
                                const noise_measurement_t *meas);

/**
 * @brief Format the primary reading line (e.g., "Noise: 68.4 dB" or "Level: 68.4% REL").
 *
 * Enforces the safety/accuracy guideline: display "dB" ONLY when calibrated.
 */
void display_format_level_line(char *buf,
                               size_t buf_len,
                               const noise_measurement_t *meas);

#ifndef HOST_TEST
#include "esp_err.h"

/**
 * @brief Initialize I2C master bus and SSD1306 128x64 OLED controller.
 *
 * If the OLED is not connected, logs a warning and returns ESP_ERR_NOT_FOUND
 * so the rest of the monitoring system can continue running cleanly.
 */
esp_err_t display_init(app_context_t *ctx);

/**
 * @brief Display a startup splash banner on the OLED.
 */
void display_show_splash(app_context_t *ctx);

/**
 * @brief Transmit the 128x64 framebuffer to the SSD1306 OLED over I2C.
 */
esp_err_t display_update(app_context_t *ctx, const uint8_t fb[OLED_FB_SIZE]);

#endif

#endif /* DISPLAY_H */
