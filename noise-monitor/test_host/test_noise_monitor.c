#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_types.h"
#include "display.h"
#include "processing.h"
#include "uart.h"
#include "web_dashboard.h"

#define PI 3.14159265358979323846

static int g_tests_passed = 0;

static bool approx_eq(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static void test_dc_only_signal_zero_rms(void)
{
    int32_t samples[256];
    for (size_t i = 0; i < 256; i++) {
        samples[i] = 1650; /* MAX4466 Vcc/2 = 1.65 V DC bias, zero audio */
    }
    float dc = 0.0f;
    float rms = noise_calc_rms(samples, 256, &dc);
    assert(approx_eq(dc, 1650.0f, 0.01f));
    assert(approx_eq(rms, 0.0f, 0.001f));
    g_tests_passed++;
}

static void test_square_wave_with_dc_bias(void)
{
    int32_t samples[256];
    const int32_t dc_bias = 1650;
    const int32_t amp = 200;
    for (size_t i = 0; i < 256; i++) {
        samples[i] = dc_bias + ((i % 2 == 0) ? amp : -amp);
    }
    float dc = 0.0f;
    float rms = noise_calc_rms(samples, 256, &dc);
    assert(approx_eq(dc, 1650.0f, 0.01f));
    assert(approx_eq(rms, 200.0f, 0.01f));
    g_tests_passed++;
}

static void test_sine_wave_rms_accuracy(void)
{
    int32_t samples[256];
    const double dc_bias = 1650.0;
    const double amp = 300.0;
    /* Exact 8 cycles across 256 samples -> RMS should equal amp / sqrt(2) */
    for (size_t i = 0; i < 256; i++) {
        double phase = (2.0 * PI * 8.0 * (double)i) / 256.0;
        samples[i] = (int32_t)llround(dc_bias + amp * sin(phase));
    }
    float dc = 0.0f;
    float rms = noise_calc_rms(samples, 256, &dc);
    float expected_rms = (float)(amp / sqrt(2.0));
    assert(approx_eq(dc, 1650.0f, 0.5f));
    assert(approx_eq(rms, expected_rms, 0.5f));
    g_tests_passed++;
}

static void test_relative_and_calibrated_db_scaling(void)
{
    /* Quiet vs loud monotonic relative scaling */
    float quiet_pct = noise_calc_relative_pct(15.0f, 450.0f);
    float mid_pct = noise_calc_relative_pct(100.0f, 450.0f);
    float loud_pct = noise_calc_relative_pct(450.0f, 450.0f);
    assert(quiet_pct < mid_pct);
    assert(mid_pct < loud_pct);
    assert(approx_eq(loud_pct, 100.0f, 0.01f));
    assert(approx_eq(noise_calc_relative_pct(0.0f, 450.0f), 0.0f, 0.01f));

    /* Calibrated SPL equation: L_p = L_ref + 20 * log10(rms / rms_ref) */
    float at_ref = noise_calc_calibrated_db(500.0f, 500.0f, 94.0f);
    float tenth_ref = noise_calc_calibrated_db(50.0f, 500.0f, 94.0f);
    float half_ref = noise_calc_calibrated_db(250.0f, 500.0f, 94.0f);
    assert(approx_eq(at_ref, 94.0f, 0.01f));
    assert(approx_eq(tenth_ref, 74.0f, 0.05f)); /* -20 dB for 1/10 amplitude */
    assert(approx_eq(half_ref, 87.98f, 0.05f)); /* -6.02 dB for 1/2 amplitude */
    g_tests_passed++;
}

static void test_hysteresis_prevents_chattering(void)
{
    const float trig = 65.0f;
    const float reset = 60.0f;

    /* Below trigger: stays inactive */
    bool active = false;
    active = noise_calc_update_hysteresis(active, 64.9f, trig, reset);
    assert(!active);

    /* Crosses trigger: activates */
    active = noise_calc_update_hysteresis(active, 65.0f, trig, reset);
    assert(active);

    /* Dips inside hysteresis band (60 < val < 65): stays active */
    active = noise_calc_update_hysteresis(active, 62.5f, trig, reset);
    assert(active);
    active = noise_calc_update_hysteresis(active, 60.1f, trig, reset);
    assert(active);

    /* Drops to or below reset threshold: clears */
    active = noise_calc_update_hysteresis(active, 60.0f, trig, reset);
    assert(!active);

    /* Rises back into hysteresis band: stays inactive until >= trigger */
    active = noise_calc_update_hysteresis(active, 64.0f, trig, reset);
    assert(!active);

    g_tests_passed++;
}

static void test_full_pipeline_mute_and_auto_unmute(void)
{
    noise_calc_state_t state;
    noise_calc_state_init(&state);

    app_runtime_config_t cfg = {
        .is_calibrated = false,
        .cal_ref_db = 94.0f,
        .cal_ref_rms = 500.0f,
        .rel_full_scale_rms = 450.0f,
        .ema_alpha = 1.0f, /* Immediate response for deterministic test */
        .threshold_trigger = 65.0f,
        .threshold_reset = 60.0f,
        .preset_index = 0,
        .muted = false,
    };

    audio_block_t loud_block = { .count = 256, .capture_timestamp_us = 100000, .seq_num = 1, .is_millivolts = true };
    audio_block_t quiet_block = { .count = 256, .capture_timestamp_us = 200000, .seq_num = 2, .is_millivolts = true };
    for (size_t i = 0; i < 256; i++) {
        loud_block.samples[i] = 1650 + ((i % 2 == 0) ? 350 : -350);
        quiet_block.samples[i] = 1650 + ((i % 2 == 0) ? 10 : -10);
    }

    noise_measurement_t meas;
    noise_calc_process_block(&state, &loud_block, &cfg, 100500, &meas);
    assert(meas.alert_active);
    assert(meas.status == NOISE_STATUS_WARNING);

    /* User presses button to mute alarm while loud noise persists */
    cfg.muted = true;
    noise_calc_process_block(&state, &loud_block, &cfg, 150500, &meas);
    assert(meas.alert_active);
    assert(meas.status == NOISE_STATUS_MUTED);

    /* Noise drops below reset threshold -> returns to NORMAL and auto-clears mute */
    noise_calc_process_block(&state, &quiet_block, &cfg, 200500, &meas);
    assert(!meas.alert_active);
    assert(!cfg.muted);
    assert(meas.status == NOISE_STATUS_NORMAL);

    g_tests_passed++;
}

static void test_display_and_logger_formatting(void)
{
    noise_measurement_t meas = {
        .seq_num = 42,
        .capture_timestamp_us = 5000000,
        .process_timestamp_us = 5000450,
        .dc_offset = 1650.0f,
        .rms_raw = 180.0f,
        .rms_smoothed = 175.5f,
        .relative_pct = 79.5f,
        .estimated_db = 84.9f,
        .primary_value = 79.5f,
        .threshold_trigger = 65.0f,
        .threshold_reset = 60.0f,
        .is_calibrated = false,
        .is_millivolts = true,
        .alert_active = true,
        .muted = false,
        .status = NOISE_STATUS_WARNING,
        .preset_name = "RESIDENT",
    };

    /* Uncalibrated mode MUST NOT display "dB" */
    char level_line[32];
    display_format_level_line(level_line, sizeof(level_line), &meas);
    assert(strstr(level_line, "REL") != NULL);
    assert(strstr(level_line, "dB") == NULL);

    /* Calibrated mode MUST display "dB" */
    meas.is_calibrated = true;
    meas.primary_value = meas.estimated_db;
    display_format_level_line(level_line, sizeof(level_line), &meas);
    assert(strstr(level_line, "dB") != NULL);

    /* Verify SSD1306 framebuffer is non-empty after rendering */
    uint8_t fb[OLED_FB_SIZE];
    display_render_measurement(fb, &meas);
    size_t lit_bytes = 0;
    for (size_t i = 0; i < OLED_FB_SIZE; i++) {
        if (fb[i] != 0) {
            lit_bytes++;
        }
    }
    assert(lit_bytes > 100);

    /* Verify human-readable and CSV logger formats */
    char log_buf[192];
    int len_txt = logger_format_record(log_buf, sizeof(log_buf), &meas, false);
    assert(len_txt > 0);
    assert(strstr(log_buf, "WARNING") != NULL);
    assert(strstr(log_buf, "84.9 dB") != NULL);

    int len_csv = logger_format_record(log_buf, sizeof(log_buf), &meas, true);
    assert(len_csv > 0);
    assert(strstr(log_buf, "5000,42,") != NULL);
    assert(strstr(log_buf, ",WARNING,450\n") != NULL);

    g_tests_passed++;
}

static void test_web_dashboard_history_and_json(void)
{
    web_history_buffer_t hist;
    web_history_init(&hist);
    assert(web_history_count(&hist) == 0);

    noise_measurement_t meas = {
        .seq_num = 1,
        .capture_timestamp_us = 1000000,
        .process_timestamp_us = 1000500,
        .dc_offset = 1650.0f,
        .rms_raw = 120.0f,
        .rms_smoothed = 118.4f,
        .relative_pct = 71.2f,
        .estimated_db = 81.5f,
        .primary_value = 71.2f,
        .threshold_trigger = 65.0f,
        .threshold_reset = 60.0f,
        .is_calibrated = false,
        .is_millivolts = true,
        .alert_active = true,
        .muted = false,
        .status = NOISE_STATUS_WARNING,
        .preset_name = "RESIDENT",
    };

    /* Push past WEB_HISTORY_CAPACITY to verify circular ring-buffer wraparound */
    for (size_t i = 0; i < WEB_HISTORY_CAPACITY + 15; i++) {
        meas.seq_num = (uint32_t)(i + 1);
        meas.process_timestamp_us = (int64_t)(i + 1) * 250000LL;
        meas.primary_value = 40.0f + (float)(i % 45);
        web_history_push(&hist, &meas);
    }
    assert(web_history_count(&hist) == WEB_HISTORY_CAPACITY);

    char live_json[512];
    int live_len = web_format_live_json(live_json, sizeof(live_json), &meas, "192.168.4.1", "ESP32-S3 Wi-Fi");
    assert(live_len > 0);
    assert(strstr(live_json, "\"ip\":\"192.168.4.1\"") != NULL);
    assert(strstr(live_json, "\"status\":\"WARNING\"") != NULL);
    assert(strstr(live_json, "\"is_calibrated\":false") != NULL);

    char hist_json[4608];
    int hist_len = web_format_history_json(hist_json, sizeof(hist_json), &hist, &meas);
    assert(hist_len > 0);
    assert(strstr(hist_json, "\"points\":[[4000,") != NULL); /* Oldest kept sample is index 15 -> 16*250ms = 4000 ms */
    assert(strstr(hist_json, "]]}") != NULL);

    /* Control JSON parser verification */
    web_control_cmd_t cmd;
    assert(web_parse_control_json("{\"mute\": true, \"preset\": 2}", &cmd));
    assert(cmd.has_mute && cmd.mute);
    assert(cmd.has_preset && cmd.preset == 2);
    assert(!cmd.has_trigger && !cmd.has_reset);

    assert(web_parse_control_json("{\"trigger\": 72.5, \"reset\": 66.0}", &cmd));
    assert(cmd.has_trigger && approx_eq(cmd.trigger, 72.5f, 0.01f));
    assert(cmd.has_reset && approx_eq(cmd.reset, 66.0f, 0.01f));
    assert(!cmd.has_mute);

    assert(!web_parse_control_json("{\"unknown\": 123}", &cmd));

    g_tests_passed++;
}

static void test_new_modular_api_names(void)
{
    int32_t samples[256];
    for (size_t i = 0; i < 256; i++) {
        samples[i] = 1650 + ((i % 2 == 0) ? 100 : -100);
    }
    float dc = 0.0f;
    float rms = processing_rms(samples, 256, &dc);
    assert(approx_eq(dc, 1650.0f, 0.01f));
    assert(approx_eq(rms, 100.0f, 0.01f));

    float pct = processing_relative_pct(100.0f, 450.0f);
    assert(pct > 0.0f && pct < 100.0f);

    float db = processing_calibrated_db(500.0f, 500.0f, 94.0f);
    assert(approx_eq(db, 94.0f, 0.01f));

    bool h = processing_update_hysteresis(false, 70.0f, 65.0f, 60.0f);
    assert(h == true);

    processing_state_t st;
    processing_state_init(&st);
    app_runtime_config_t cfg = {
        .is_calibrated = false,
        .cal_ref_db = 94.0f,
        .cal_ref_rms = 500.0f,
        .rel_full_scale_rms = 450.0f,
        .ema_alpha = 0.25f,
        .threshold_trigger = 65.0f,
        .threshold_reset = 60.0f,
        .preset_index = 0,
        .muted = false,
    };
    audio_block_t b = { .count = 256, .capture_timestamp_us = 1000, .seq_num = 1, .is_millivolts = true };
    memcpy(b.samples, samples, sizeof(samples));
    noise_measurement_t m;
    processing_block(&st, &b, &cfg, 2000, &m);
    assert(approx_eq(m.rms_raw, 100.0f, 0.01f));

    char buf[128];
    int len = uart_format_record(buf, sizeof(buf), &m, true);
    assert(len > 0);

    g_tests_passed++;
}

int main(void)
{
    test_dc_only_signal_zero_rms();
    test_square_wave_with_dc_bias();
    test_sine_wave_rms_accuracy();
    test_relative_and_calibrated_db_scaling();
    test_hysteresis_prevents_chattering();
    test_full_pipeline_mute_and_auto_unmute();
    test_display_and_logger_formatting();
    test_web_dashboard_history_and_json();
    test_new_modular_api_names();

    printf("ALL %d HOST UNIT TESTS PASSED!\n", g_tests_passed);
    return 0;
}
