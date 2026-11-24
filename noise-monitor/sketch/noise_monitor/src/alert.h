#ifndef ALERT_H
#define ALERT_H

#include "app_types.h"

#ifndef HOST_TEST
#include "esp_err.h"

/**
 * @brief Initialize Green LED, Red LED, Buzzer (GPIO or PWM), and Push Button ISR.
 */
esp_err_t alert_init(app_context_t *ctx);

/**
 * @brief Update physical LED and buzzer outputs for the given noise status.
 */
void alert_apply_state(noise_status_t status, bool pulse_on);

/**
 * @brief Handle push button press (mute during warning; preset cycle during normal).
 */
void alert_handle_button_press(app_context_t *ctx, const noise_measurement_t *latest);

/* Backward compatibility aliases */
static inline esp_err_t alarm_init(app_context_t *ctx) { return alert_init(ctx); }
static inline void alarm_apply_state(noise_status_t s, bool p) { alert_apply_state(s, p); }
#endif

#endif /* ALERT_H */
