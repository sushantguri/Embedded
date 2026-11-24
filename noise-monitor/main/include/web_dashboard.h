#ifndef WEB_DASHBOARD_H
#define WEB_DASHBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#ifndef HOST_TEST
#include "esp_err.h"
#endif

#define WEB_HISTORY_CAPACITY 120

/**
 * @brief Compact point stored in the in-RAM circular history buffer.
 */
typedef struct {
    int64_t uptime_ms;
    float primary_value;
    float rms_smoothed;
    float threshold_trigger;
    uint8_t status_code;
} web_history_point_t;

/**
 * @brief Fixed-capacity circular history buffer for live line charts.
 */
typedef struct {
    web_history_point_t points[WEB_HISTORY_CAPACITY];
    size_t head;
    size_t count;
    int64_t last_recorded_us;
} web_history_buffer_t;

/**
 * @brief Parsed fields from POST /api/control JSON payload.
 */
typedef struct {
    bool has_mute;
    bool mute;
    bool has_preset;
    int preset;
    bool has_trigger;
    float trigger;
    bool has_reset;
    float reset;
} web_control_cmd_t;

/* ---- Pure C helpers (host-testable) ---- */
void web_history_init(web_history_buffer_t *buf);
void web_history_push(web_history_buffer_t *buf, const noise_measurement_t *meas);
size_t web_history_count(const web_history_buffer_t *buf);

int web_format_live_json(char *out,
                         size_t out_len,
                         const noise_measurement_t *meas,
                         const char *ip_str,
                         const char *source_label);

int web_format_history_json(char *out,
                            size_t out_len,
                            const web_history_buffer_t *buf,
                            const noise_measurement_t *latest);

bool web_parse_control_json(const char *json, web_control_cmd_t *out_cmd);

#ifndef HOST_TEST
/**
 * @brief Initialize Wi-Fi (SoftAP or STA) and start ESP-IDF HTTP dashboard server.
 */
esp_err_t web_dashboard_start(app_context_t *ctx);

/**
 * @brief Publish the latest measurement to the dashboard snapshot and history ring buffer.
 *        Safe to call from noise_processing_task; uses a short mutex timeout so DSP never blocks.
 */
void web_dashboard_publish(const noise_measurement_t *meas);

/**
 * @brief Get current IPv4 address string (e.g. "192.168.4.1" or STA IP).
 */
void web_dashboard_get_ip(char *out, size_t out_len);
#endif

#endif /* WEB_DASHBOARD_H */
