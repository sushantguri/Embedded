#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#ifndef HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#endif

typedef enum {
    NOISE_STATUS_NORMAL = 0,
    NOISE_STATUS_WARNING = 1,
    NOISE_STATUS_MUTED = 2,
} noise_status_t;

/**
 * @brief Raw audio sample block produced by Audio Sampling Task.
 */
typedef struct {
    int32_t samples[APP_AUDIO_BLOCK_SAMPLES];
    size_t count;
    int64_t capture_timestamp_us;
    uint32_t seq_num;
    bool is_millivolts;
} audio_block_t;

/**
 * @brief Threshold preset descriptor for quick environment selection.
 */
typedef struct {
    const char *name;
    float trigger_threshold;
    float reset_threshold;
} noise_threshold_preset_t;

/**
 * @brief Processed noise measurement fanned out to Display, Alert, and Logger tasks.
 */
typedef struct {
    uint32_t seq_num;
    int64_t capture_timestamp_us;
    int64_t process_timestamp_us;
    float dc_offset;
    float rms_raw;
    float rms_smoothed;
    float relative_pct;
    float estimated_db;
    float primary_value;          /* relative_pct if uncalibrated, estimated_db if calibrated */
    float threshold_trigger;
    float threshold_reset;
    bool is_calibrated;
    bool is_millivolts;
    bool alert_active;
    bool muted;
    noise_status_t status;
    const char *preset_name;
} noise_measurement_t;

/**
 * @brief Runtime configuration shared safely across tasks via mutex.
 */
typedef struct {
    bool is_calibrated;
    float cal_ref_db;
    float cal_ref_rms;
    float rel_full_scale_rms;
    float ema_alpha;
    float threshold_trigger;
    float threshold_reset;
    size_t preset_index;
    bool muted;
    uint32_t dropped_audio_blocks;
    uint32_t dropped_log_packets;
} app_runtime_config_t;

#ifndef HOST_TEST
/**
 * @brief Shared FreeRTOS synchronization and IPC handles.
 */
typedef struct {
    QueueHandle_t raw_audio_queue;  /* Audio Sampling -> Processing */
    QueueHandle_t display_queue;    /* Processing -> Display (len 1, overwrite) */
    QueueHandle_t alert_queue;      /* Processing -> Alert (len 1, overwrite) */
    QueueHandle_t logger_queue;     /* Processing -> Logger (buffered) */
    SemaphoreHandle_t button_sem;   /* Push button GPIO ISR -> Alert Task */
    SemaphoreHandle_t state_mutex;  /* Protects shared runtime config */
    SemaphoreHandle_t i2c_mutex;    /* Protects shared I2C bus */
    app_runtime_config_t config;
} app_context_t;
#endif

static inline const char *noise_status_to_str(noise_status_t status)
{
    switch (status) {
        case NOISE_STATUS_NORMAL:
            return "NORMAL";
        case NOISE_STATUS_WARNING:
            return "WARNING";
        case NOISE_STATUS_MUTED:
            return "MUTED";
        default:
            return "UNKNOWN";
    }
}

#endif /* APP_TYPES_H */
