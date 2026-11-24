#include "processing.h"

#include <math.h>
#include <stddef.h>

/* Built-in monitoring environment presets (selectable via push button or dashboard) */
static const noise_threshold_preset_t s_presets[] = {
    { .name = "DEFAULT",    .trigger_threshold = APP_THRESHOLD_TRIGGER_DEFAULT, .reset_threshold = APP_THRESHOLD_RESET_DEFAULT },
    { .name = "QUIET/LIB",  .trigger_threshold = 45.0f,                         .reset_threshold = 40.0f },
    { .name = "RESIDENT",   .trigger_threshold = 65.0f,                         .reset_threshold = 60.0f },
    { .name = "INDUSTRIAL", .trigger_threshold = 80.0f,                         .reset_threshold = 74.0f },
};

const noise_threshold_preset_t *processing_get_presets(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(s_presets) / sizeof(s_presets[0]);
    }
    return s_presets;
}

void processing_state_init(processing_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->initialized = false;
    state->rms_ema = 0.0f;
    state->alert_latched = false;
}

float processing_rms(const int32_t *samples, size_t count, float *out_dc)
{
    if (samples == NULL || count == 0) {
        if (out_dc != NULL) {
            *out_dc = 0.0f;
        }
        return 0.0f;
    }

    /* Pass 1: Compute mean (DC offset) */
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += (double)samples[i];
    }
    double mean = sum / (double)count;

    if (out_dc != NULL) {
        *out_dc = (float)mean;
    }

    /* Pass 2: Remove DC offset and compute RMS = sqrt((1/N) * sum(x_i^2)) */
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; i++) {
        double centered = (double)samples[i] - mean;
        sum_sq += centered * centered;
    }

    return (float)sqrt(sum_sq / (double)count);
}

float processing_relative_pct(float rms, float full_scale_rms)
{
    if (rms <= 0.0f || full_scale_rms <= 0.0f) {
        return 0.0f;
    }

    /*
     * Map RMS over a 40 dB dynamic range (1% of full-scale -> 0%,
     * 100% of full-scale -> 100%) so human-perceived loudness changes
     * respond smoothly across quiet and loud environments.
     */
    const float min_ratio = 0.01f; /* -40 dB floor relative to full scale */
    float ratio = rms / full_scale_rms;
    if (ratio <= min_ratio) {
        return 0.0f;
    }
    if (ratio >= 1.0f) {
        return 100.0f;
    }

    float db_below_fs = 20.0f * log10f(ratio); /* Range [-40.0, 0.0] */
    float pct = ((db_below_fs + 40.0f) / 40.0f) * 100.0f;
    if (pct < 0.0f) {
        return 0.0f;
    }
    if (pct > 100.0f) {
        return 100.0f;
    }
    return pct;
}

float processing_calibrated_db(float rms, float cal_ref_rms, float cal_ref_db)
{
    /* Floor RMS to prevent log10(0) -> -inf in digital silence */
    const float rms_floor = 0.1f;
    float safe_rms = (rms > rms_floor) ? rms : rms_floor;
    float safe_ref = (cal_ref_rms > rms_floor) ? cal_ref_rms : 1.0f;

    /*
     * Equivalent to L_p = 20 * log10(p_rms / 20 uPa) once the sensor sensitivity
     * (p_ref -> cal_ref_rms at cal_ref_db) is measured against a reference meter.
     */
    float db = cal_ref_db + 20.0f * log10f(safe_rms / safe_ref);
    if (db < 20.0f) {
        db = 20.0f;
    } else if (db > 130.0f) {
        db = 130.0f;
    }
    return db;
}

bool processing_update_hysteresis(bool currently_active,
                                  float value,
                                  float trigger_threshold,
                                  float reset_threshold)
{
    /* Guard against misconfigured reset >= trigger */
    float safe_reset = (reset_threshold < trigger_threshold) ? reset_threshold : trigger_threshold;

    if (!currently_active) {
        return (value >= trigger_threshold);
    }
    return (value > safe_reset);
}

void processing_block(processing_state_t *state,
                      const audio_block_t *block,
                      app_runtime_config_t *cfg,
                      int64_t now_us,
                      noise_measurement_t *out_meas)
{
    if (state == NULL || block == NULL || cfg == NULL || out_meas == NULL) {
        return;
    }

    float dc = 0.0f;
    float rms_raw = processing_rms(block->samples, block->count, &dc);

    float alpha = cfg->ema_alpha;
    if (alpha <= 0.0f || alpha > 1.0f) {
        alpha = 0.25f;
    }

    if (!state->initialized) {
        state->rms_ema = rms_raw;
        state->initialized = true;
    } else {
        state->rms_ema = (alpha * rms_raw) + ((1.0f - alpha) * state->rms_ema);
    }

    float rel_pct = processing_relative_pct(state->rms_ema, cfg->rel_full_scale_rms);
    float est_db = processing_calibrated_db(state->rms_ema, cfg->cal_ref_rms, cfg->cal_ref_db);
    float primary = cfg->is_calibrated ? est_db : rel_pct;

    bool prev_active = state->alert_latched;
    state->alert_latched = processing_update_hysteresis(prev_active,
                                                        primary,
                                                        cfg->threshold_trigger,
                                                        cfg->threshold_reset);

    /* Automatically clear mute once sound returns safely below reset threshold */
    if (!state->alert_latched) {
        cfg->muted = false;
    }

    noise_status_t status = NOISE_STATUS_NORMAL;
    if (state->alert_latched) {
        status = cfg->muted ? NOISE_STATUS_MUTED : NOISE_STATUS_WARNING;
    }

    size_t preset_count = 0;
    const noise_threshold_preset_t *presets = processing_get_presets(&preset_count);
    const char *preset_name = (cfg->preset_index < preset_count)
                                  ? presets[cfg->preset_index].name
                                  : "CUSTOM";

    out_meas->seq_num = block->seq_num;
    out_meas->capture_timestamp_us = block->capture_timestamp_us;
    out_meas->process_timestamp_us = now_us;
    out_meas->dc_offset = dc;
    out_meas->rms_raw = rms_raw;
    out_meas->rms_smoothed = state->rms_ema;
    out_meas->relative_pct = rel_pct;
    out_meas->estimated_db = est_db;
    out_meas->primary_value = primary;
    out_meas->threshold_trigger = cfg->threshold_trigger;
    out_meas->threshold_reset = cfg->threshold_reset;
    out_meas->is_calibrated = cfg->is_calibrated;
    out_meas->is_millivolts = block->is_millivolts;
    out_meas->alert_active = state->alert_latched;
    out_meas->muted = cfg->muted;
    out_meas->status = status;
    out_meas->preset_name = preset_name;
}
