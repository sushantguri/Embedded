#ifndef PROCESSING_H
#define PROCESSING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

/**
 * @brief Stateful DSP + hysteresis context for noise level computation.
 */
typedef struct {
    bool initialized;
    float rms_ema;
    bool alert_latched;
} processing_state_t;

typedef processing_state_t noise_calc_state_t;

/**
 * @brief Initialize noise processing state.
 */
void processing_state_init(processing_state_t *state);

/**
 * @brief Compute DC offset and RMS amplitude of a sample block.
 *
 * Implements:
 *   mu    = (1/N) * sum(s[i])
 *   x_rms = sqrt((1/N) * sum((s[i] - mu)^2))
 */
float processing_rms(const int32_t *samples, size_t count, float *out_dc);

/**
 * @brief Convert RMS amplitude to relative noise percentage [0.0, 100.0].
 */
float processing_relative_pct(float rms, float full_scale_rms);

/**
 * @brief Convert RMS amplitude to calibrated sound pressure level (dB SPL).
 */
float processing_calibrated_db(float rms, float cal_ref_rms, float cal_ref_db);

/**
 * @brief Update hysteresis alert comparator.
 */
bool processing_update_hysteresis(bool currently_active,
                                  float value,
                                  float trigger_threshold,
                                  float reset_threshold);

/**
 * @brief Full processing pipeline for one captured audio block.
 */
void processing_block(processing_state_t *state,
                      const audio_block_t *block,
                      app_runtime_config_t *cfg,
                      int64_t now_us,
                      noise_measurement_t *out_meas);

/**
 * @brief Return pointer to built-in monitoring environment threshold presets.
 */
const noise_threshold_preset_t *processing_get_presets(size_t *out_count);

/* Backward compatibility aliases */
static inline void noise_calc_state_init(noise_calc_state_t *s) { processing_state_init(s); }
static inline float noise_calc_rms(const int32_t *s, size_t c, float *d) { return processing_rms(s, c, d); }
static inline float noise_calc_relative_pct(float r, float f) { return processing_relative_pct(r, f); }
static inline float noise_calc_calibrated_db(float r, float cr, float cd) { return processing_calibrated_db(r, cr, cd); }
static inline bool noise_calc_update_hysteresis(bool a, float v, float t, float rt) { return processing_update_hysteresis(a, v, t, rt); }
static inline void noise_calc_process_block(noise_calc_state_t *s, const audio_block_t *b, app_runtime_config_t *c, int64_t n, noise_measurement_t *m) {
    processing_block(s, b, c, n, m);
}
static inline const noise_threshold_preset_t *noise_calc_get_presets(size_t *c) { return processing_get_presets(c); }

#endif /* PROCESSING_H */
