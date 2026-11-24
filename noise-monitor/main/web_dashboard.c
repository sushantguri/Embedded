#include "web_dashboard.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "noise_calculation.h"

#ifndef HOST_TEST
#ifndef ARDUINO
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "web_dash";

#if defined(EMBED_HTML_HEADER)
#include "index_html.h"
#else
/* Embedded single-page dashboard HTML (main/web/index.html) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
#endif
#else /* ARDUINO */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "web_dash";
#endif /* ARDUINO */
#endif /* HOST_TEST */

void web_history_init(web_history_buffer_t *buf)
{
    if (buf == NULL) {
        return;
    }
    memset(buf, 0, sizeof(*buf));
}

void web_history_push(web_history_buffer_t *buf, const noise_measurement_t *meas)
{
    if (buf == NULL || meas == NULL) {
        return;
    }

    web_history_point_t pt = {
        .uptime_ms = meas->process_timestamp_us / 1000LL,
        .primary_value = meas->primary_value,
        .rms_smoothed = meas->rms_smoothed,
        .threshold_trigger = meas->threshold_trigger,
        .status_code = (uint8_t)meas->status,
    };

    buf->points[buf->head] = pt;
    buf->head = (buf->head + 1U) % WEB_HISTORY_CAPACITY;
    if (buf->count < WEB_HISTORY_CAPACITY) {
        buf->count++;
    }
    buf->last_recorded_us = meas->process_timestamp_us;
}

size_t web_history_count(const web_history_buffer_t *buf)
{
    return (buf != NULL) ? buf->count : 0;
}

int web_format_live_json(char *out,
                         size_t out_len,
                         const noise_measurement_t *meas,
                         const char *ip_str,
                         const char *source_label)
{
    if (out == NULL || out_len == 0 || meas == NULL) {
        return 0;
    }

    int64_t uptime_ms = meas->process_timestamp_us / 1000LL;
    int64_t latency_us = meas->process_timestamp_us - meas->capture_timestamp_us;
    if (latency_us < 0) {
        latency_us = 0;
    }

    const char *preset = (meas->preset_name != NULL) ? meas->preset_name : "CUSTOM";
    const char *ip = (ip_str != NULL) ? ip_str : "";
    const char *src = (source_label != NULL) ? source_label : "ESP32-S3 Wi-Fi";

    return snprintf(
        out,
        out_len,
        "{\"seq\":%" PRIu32
        ",\"uptime_ms\":%" PRId64
        ",\"primary_value\":%.2f"
        ",\"relative_pct\":%.2f"
        ",\"estimated_db\":%.2f"
        ",\"rms_raw\":%.2f"
        ",\"rms_smoothed\":%.2f"
        ",\"dc_offset\":%.1f"
        ",\"threshold_trigger\":%.1f"
        ",\"threshold_reset\":%.1f"
        ",\"is_calibrated\":%s"
        ",\"is_millivolts\":%s"
        ",\"alert_active\":%s"
        ",\"muted\":%s"
        ",\"status\":\"%s\""
        ",\"preset_name\":\"%s\""
        ",\"latency_us\":%" PRId64
        ",\"ip\":\"%s\""
        ",\"source\":\"%s\"}",
        meas->seq_num,
        uptime_ms,
        (double)meas->primary_value,
        (double)meas->relative_pct,
        (double)meas->estimated_db,
        (double)meas->rms_raw,
        (double)meas->rms_smoothed,
        (double)meas->dc_offset,
        (double)meas->threshold_trigger,
        (double)meas->threshold_reset,
        meas->is_calibrated ? "true" : "false",
        meas->is_millivolts ? "true" : "false",
        meas->alert_active ? "true" : "false",
        meas->muted ? "true" : "false",
        noise_status_to_str(meas->status),
        preset,
        latency_us,
        ip,
        src);
}

int web_format_history_json(char *out,
                            size_t out_len,
                            const web_history_buffer_t *buf,
                            const noise_measurement_t *latest)
{
    if (out == NULL || out_len == 0 || buf == NULL) {
        return 0;
    }

    bool is_cal = (latest != NULL) ? latest->is_calibrated : false;
    bool is_mv = (latest != NULL) ? latest->is_millivolts : true;
    float reset_thr = (latest != NULL) ? latest->threshold_reset : APP_THRESHOLD_RESET_DEFAULT;
    const char *preset = (latest != NULL && latest->preset_name != NULL) ? latest->preset_name : "DEFAULT";

    size_t pos = 0;
    int n = snprintf(
        out + pos,
        out_len - pos,
        "{\"is_calibrated\":%s,\"is_millivolts\":%s,\"threshold_reset\":%.1f,\"preset_name\":\"%s\",\"points\":[",
        is_cal ? "true" : "false",
        is_mv ? "true" : "false",
        (double)reset_thr,
        preset);
    if (n < 0 || (size_t)n >= out_len - pos) {
        return (int)pos;
    }
    pos += (size_t)n;

    size_t start = (buf->count == WEB_HISTORY_CAPACITY) ? buf->head : 0U;
    for (size_t i = 0; i < buf->count; i++) {
        const web_history_point_t *pt = &buf->points[(start + i) % WEB_HISTORY_CAPACITY];
        n = snprintf(
            out + pos,
            out_len - pos,
            "%s[%" PRId64 ",%.1f,%.1f,%.1f,%u]",
            (i == 0) ? "" : ",",
            pt->uptime_ms,
            (double)pt->primary_value,
            (double)pt->rms_smoothed,
            (double)pt->threshold_trigger,
            (unsigned)pt->status_code);
        if (n < 0 || (size_t)n >= out_len - pos - 3U) {
            break;
        }
        pos += (size_t)n;
    }

    n = snprintf(out + pos, out_len - pos, "]}");
    if (n > 0 && (size_t)n < out_len - pos) {
        pos += (size_t)n;
    }
    return (int)pos;
}

static const char *find_json_key_value(const char *json, const char *key)
{
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) {
        return NULL;
    }

    const char *p = strstr(json, needle);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(needle);
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

bool web_parse_control_json(const char *json, web_control_cmd_t *out_cmd)
{
    if (json == NULL || out_cmd == NULL) {
        return false;
    }
    memset(out_cmd, 0, sizeof(*out_cmd));
    bool any = false;

    const char *v = find_json_key_value(json, "mute");
    if (v != NULL) {
        if (strncmp(v, "true", 4) == 0 || *v == '1') {
            out_cmd->has_mute = true;
            out_cmd->mute = true;
            any = true;
        } else if (strncmp(v, "false", 5) == 0 || *v == '0') {
            out_cmd->has_mute = true;
            out_cmd->mute = false;
            any = true;
        }
    }

    v = find_json_key_value(json, "preset");
    if (v != NULL) {
        char *endp = NULL;
        long val = strtol(v, &endp, 10);
        if (endp != v) {
            out_cmd->has_preset = true;
            out_cmd->preset = (int)val;
            any = true;
        }
    }

    v = find_json_key_value(json, "trigger");
    if (v != NULL) {
        char *endp = NULL;
        float val = strtof(v, &endp);
        if (endp != v) {
            out_cmd->has_trigger = true;
            out_cmd->trigger = val;
            any = true;
        }
    }

    v = find_json_key_value(json, "reset");
    if (v != NULL) {
        char *endp = NULL;
        float val = strtof(v, &endp);
        if (endp != v) {
            out_cmd->has_reset = true;
            out_cmd->reset = val;
            any = true;
        }
    }

    return any;
}

#ifndef HOST_TEST

#ifdef ARDUINO

/*
 * Under Arduino, the .ino file owns Wi-Fi + WebServer.
 * web_dashboard.c only provides the publish/get_ip helpers
 * that write to extern globals declared in the .ino.
 */
extern SemaphoreHandle_t arduino_web_mutex;
extern noise_measurement_t arduino_latest_meas;
extern bool arduino_has_meas;
extern web_history_buffer_t arduino_history;

esp_err_t web_dashboard_start(app_context_t *ctx)
{
    (void)ctx;
    /* Wi-Fi + HTTP server already started by noise_monitor.ino setup() */
    return ESP_OK;
}

void web_dashboard_publish(const noise_measurement_t *meas)
{
    if (meas == NULL || arduino_web_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(arduino_web_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    arduino_latest_meas = *meas;
    arduino_has_meas = true;

    bool status_changed = (arduino_history.count > 0) &&
                          (arduino_history.points[(arduino_history.head + WEB_HISTORY_CAPACITY - 1U) % WEB_HISTORY_CAPACITY].status_code !=
                           (uint8_t)meas->status);
    bool interval_due = (arduino_history.count == 0) ||
                        ((meas->process_timestamp_us - arduino_history.last_recorded_us) >=
                         ((int64_t)APP_WEB_HISTORY_INTERVAL_MS * 1000LL));

    if (status_changed || interval_due) {
        web_history_push(&arduino_history, meas);
    }

    xSemaphoreGive(arduino_web_mutex);
}

void web_dashboard_get_ip(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    strncpy(out, "192.168.4.1", out_len - 1);
    out[out_len - 1] = '\0';
}

#else /* Native ESP-IDF build */

static app_context_t *s_app_ctx = NULL;
static SemaphoreHandle_t s_web_mutex = NULL;
static web_history_buffer_t s_history;
static noise_measurement_t s_latest_meas;
static bool s_has_meas = false;
static char s_ip_str[32] = "192.168.4.1";
static httpd_handle_t s_httpd = NULL;

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t handle_root_get(httpd_req_t *req)
{
#if defined(ARDUINO) || defined(EMBED_HTML_HEADER)
    const char *html_data = INDEX_HTML_STR;
    size_t html_len = strlen(INDEX_HTML_STR);
#else
    const char *html_data = (const char *)index_html_start;
    size_t html_len = (size_t)(index_html_end - index_html_start);
    /* Exclude trailing NUL byte added by EMBED_TXTFILES if present */
    if (html_len > 0 && index_html_start[html_len - 1] == '\0') {
        html_len--;
    }
#endif

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html_data, (ssize_t)html_len);
}

static esp_err_t handle_live_get(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    noise_measurement_t snap;
    bool valid = false;
    char ip_copy[32];

    if (s_web_mutex != NULL && xSemaphoreTake(s_web_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snap = s_latest_meas;
        valid = s_has_meas;
        strncpy(ip_copy, s_ip_str, sizeof(ip_copy) - 1);
        ip_copy[sizeof(ip_copy) - 1] = '\0';
        xSemaphoreGive(s_web_mutex);
    } else {
        return httpd_resp_send_500(req);
    }

    if (!valid) {
        memset(&snap, 0, sizeof(snap));
        snap.threshold_trigger = APP_THRESHOLD_TRIGGER_DEFAULT;
        snap.threshold_reset = APP_THRESHOLD_RESET_DEFAULT;
        snap.is_calibrated = APP_CALIBRATED_MODE_DEFAULT;
        snap.is_millivolts = true;
        snap.status = NOISE_STATUS_NORMAL;
        snap.preset_name = "DEFAULT";
    }

    char json[512];
    int len = web_format_live_json(json, sizeof(json), &snap, ip_copy, "ESP32-S3 Wi-Fi");
    return httpd_resp_send(req, json, len);
}

static esp_err_t handle_history_get(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    /* Allocate temporary JSON buffer on heap so HTTP task stack stays small */
    const size_t buf_sz = 4608;
    char *json = (char *)malloc(buf_sz);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }

    int len = 0;
    if (s_web_mutex != NULL && xSemaphoreTake(s_web_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        len = web_format_history_json(json, buf_sz, &s_history, s_has_meas ? &s_latest_meas : NULL);
        xSemaphoreGive(s_web_mutex);
    } else {
        free(json);
        return httpd_resp_send_500(req);
    }

    esp_err_t ret = httpd_resp_send(req, json, len);
    free(json);
    return ret;
}

static esp_err_t handle_control_options(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_control_post(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char body[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid body length\"}");
    }

    int received = httpd_req_recv(req, body, (size_t)total);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"read failed\"}");
    }
    body[received] = '\0';

    web_control_cmd_t cmd;
    if (!web_parse_control_json(body, &cmd)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unrecognized command\"}");
    }

    if (s_app_ctx == NULL || xSemaphoreTake(s_app_ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return httpd_resp_send_500(req);
    }

    if (cmd.has_mute) {
        s_app_ctx->config.muted = cmd.mute;
        ESP_LOGI(TAG, "Web control: mute=%s", cmd.mute ? "true" : "false");
    }

    if (cmd.has_preset) {
        size_t count = 0;
        const noise_threshold_preset_t *presets = noise_calc_get_presets(&count);
        if (cmd.preset >= 0 && (size_t)cmd.preset < count) {
            s_app_ctx->config.preset_index = (size_t)cmd.preset;
            s_app_ctx->config.threshold_trigger = presets[cmd.preset].trigger_threshold;
            s_app_ctx->config.threshold_reset = presets[cmd.preset].reset_threshold;
            ESP_LOGI(TAG, "Web control: preset=%s (%.1f / %.1f)",
                     presets[cmd.preset].name,
                     (double)s_app_ctx->config.threshold_trigger,
                     (double)s_app_ctx->config.threshold_reset);
        }
    }

    if (cmd.has_trigger && cmd.has_reset &&
        cmd.trigger >= 10.0f && cmd.trigger <= 125.0f &&
        cmd.reset >= 5.0f && cmd.reset < cmd.trigger) {
        size_t count = 0;
        (void)noise_calc_get_presets(&count);
        s_app_ctx->config.preset_index = count; /* Marks CUSTOM preset */
        s_app_ctx->config.threshold_trigger = cmd.trigger;
        s_app_ctx->config.threshold_reset = cmd.reset;
        ESP_LOGI(TAG, "Web control: custom thresholds trigger=%.1f reset=%.1f",
                 (double)cmd.trigger, (double)cmd.reset);
    }

    xSemaphoreGive(s_app_ctx->state_mutex);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi STA disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (s_web_mutex != NULL && xSemaphoreTake(s_web_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
            xSemaphoreGive(s_web_mutex);
        }
        ESP_LOGI(TAG, "Wi-Fi STA connected! Open dashboard on phone/laptop: http://" IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

#ifdef ARDUINO
static esp_err_t init_wifi(void)
{
    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "Wi-Fi SoftAP running via Arduino WiFi. IP: %s", s_ip_str);
    return ESP_OK;
}
#else
static esp_err_t init_wifi(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

#if APP_WEB_WIFI_STA
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, APP_WEB_STA_SSID, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, APP_WEB_STA_PASS, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = (strlen(APP_WEB_STA_PASS) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    snprintf(s_ip_str, sizeof(s_ip_str), "connecting...");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA connecting to SSID '%s'...", APP_WEB_STA_SSID);
#else
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, APP_WEB_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(APP_WEB_AP_SSID);
    strncpy((char *)ap_cfg.ap.password, APP_WEB_AP_PASS, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = (strlen(APP_WEB_AP_PASS) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi SoftAP ready! Connect phone/laptop to SSID '%s' (pass: '%s')",
             APP_WEB_AP_SSID, APP_WEB_AP_PASS);
    ESP_LOGI(TAG, "Open live dashboard at: http://192.168.4.1");
#endif

    return ESP_OK;
}
#endif

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = APP_WEB_HTTP_PORT;
    cfg.core_id = 0; /* Keep HTTP I/O on Core 0 so Core 1 stays real-time for audio/alert */
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;

    esp_err_t ret = httpd_start(&s_httpd, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root_get,
    };
    static const httpd_uri_t uri_live = {
        .uri = "/api/live",
        .method = HTTP_GET,
        .handler = handle_live_get,
    };
    static const httpd_uri_t uri_history = {
        .uri = "/api/history",
        .method = HTTP_GET,
        .handler = handle_history_get,
    };
    static const httpd_uri_t uri_ctrl_post = {
        .uri = "/api/control",
        .method = HTTP_POST,
        .handler = handle_control_post,
    };
    static const httpd_uri_t uri_ctrl_opt = {
        .uri = "/api/control",
        .method = HTTP_OPTIONS,
        .handler = handle_control_options,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uri_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uri_live));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uri_history));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uri_ctrl_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uri_ctrl_opt));

    ESP_LOGI(TAG, "Embedded HTTP dashboard listening on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t web_dashboard_start(app_context_t *ctx)
{
    s_app_ctx = ctx;
    web_history_init(&s_history);
    s_has_meas = false;

    s_web_mutex = xSemaphoreCreateMutex();
    if (s_web_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = init_wifi();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return start_http_server();
}

void web_dashboard_publish(const noise_measurement_t *meas)
{
    if (meas == NULL || s_web_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_web_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return; /* Never stall real-time processing task */
    }

    s_latest_meas = *meas;
    s_has_meas = true;

    bool status_changed = (s_history.count > 0) &&
                          (s_history.points[(s_history.head + WEB_HISTORY_CAPACITY - 1U) % WEB_HISTORY_CAPACITY].status_code !=
                           (uint8_t)meas->status);
    bool interval_due = (s_history.count == 0) ||
                        ((meas->process_timestamp_us - s_history.last_recorded_us) >=
                         ((int64_t)APP_WEB_HISTORY_INTERVAL_MS * 1000LL));

    if (status_changed || interval_due) {
        web_history_push(&s_history, meas);
    }

    xSemaphoreGive(s_web_mutex);
}

void web_dashboard_get_ip(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (s_web_mutex != NULL && xSemaphoreTake(s_web_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        strncpy(out, s_ip_str, out_len - 1);
        out[out_len - 1] = '\0';
        xSemaphoreGive(s_web_mutex);
    } else {
        strncpy(out, "192.168.4.1", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

#endif /* ARDUINO */
#endif /* HOST_TEST */
