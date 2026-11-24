#include "uart.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"

#ifndef HOST_TEST
#include "esp_log.h"
#if APP_USE_AUX_UART
#include "driver/uart.h"
#endif

#ifdef ARDUINO
extern void arduino_serial_write(const char *buf, size_t len);
#endif

static const char *TAG = "uart";
#endif

int uart_format_record(char *buf,
                       size_t buf_len,
                       const noise_measurement_t *meas,
                       bool csv_mode)
{
    if (buf == NULL || buf_len == 0 || meas == NULL) {
        return 0;
    }

    int64_t uptime_ms = meas->process_timestamp_us / 1000LL;
    int64_t latency_us = meas->process_timestamp_us - meas->capture_timestamp_us;
    if (latency_us < 0) {
        latency_us = 0;
    }

    if (csv_mode) {
        return snprintf(buf, buf_len,
                        "%" PRId64 ",%" PRIu32 ",%.2f,%.2f,%.1f,%.1f,%d,%.1f,%s,%" PRId64 "\n",
                        uptime_ms,
                        meas->seq_num,
                        (double)meas->rms_raw,
                        (double)meas->rms_smoothed,
                        (double)meas->relative_pct,
                        (double)meas->estimated_db,
                        meas->is_calibrated ? 1 : 0,
                        (double)meas->threshold_trigger,
                        noise_status_to_str(meas->status),
                        latency_us);
    }

    int64_t total_sec = uptime_ms / 1000LL;
    int hours = (int)(total_sec / 3600LL);
    int mins = (int)((total_sec % 3600LL) / 60LL);
    int secs = (int)(total_sec % 60LL);
    int ms = (int)(uptime_ms % 1000LL);

    if (meas->is_calibrated) {
        return snprintf(buf, buf_len,
                        "[%02d:%02d:%02d.%03d] #%" PRIu32
                        " | Noise: %5.1f dB | RMS: %6.1f %s | Thr: %4.1f (%s) | Status: %-7s | Lat: %.2f ms\n",
                        hours, mins, secs, ms,
                        meas->seq_num,
                        (double)meas->estimated_db,
                        (double)meas->rms_smoothed,
                        meas->is_millivolts ? "mV" : "cnt",
                        (double)meas->threshold_trigger,
                        meas->preset_name != NULL ? meas->preset_name : "CUSTOM",
                        noise_status_to_str(meas->status),
                        (double)latency_us / 1000.0);
    }

    return snprintf(buf, buf_len,
                    "[%02d:%02d:%02d.%03d] #%" PRIu32
                    " | Level: %5.1f%% (REL) | RMS: %6.1f %s | Thr: %4.1f (%s) | Status: %-7s | Lat: %.2f ms\n",
                    hours, mins, secs, ms,
                    meas->seq_num,
                    (double)meas->relative_pct,
                    (double)meas->rms_smoothed,
                    meas->is_millivolts ? "mV" : "cnt",
                    (double)meas->threshold_trigger,
                    meas->preset_name != NULL ? meas->preset_name : "CUSTOM",
                    noise_status_to_str(meas->status),
                    (double)latency_us / 1000.0);
}

#ifndef HOST_TEST

esp_err_t uart_init(void)
{
#if APP_USE_AUX_UART
    const uart_config_t uart_cfg = {
        .baud_rate = APP_AUX_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(UART_NUM_1, 256, 1024, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART1 driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = uart_param_config(UART_NUM_1, &uart_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_set_pin(UART_NUM_1, APP_AUX_UART_TX_GPIO, APP_AUX_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "Auxiliary UART1 ready (TX=%d, RX=%d, %d baud)",
             APP_AUX_UART_TX_GPIO, APP_AUX_UART_RX_GPIO, APP_AUX_UART_BAUD);
#else
    ESP_LOGI(TAG, "UART initialized on default console (interval=%d ms, csv=%s)",
             APP_LOG_INTERVAL_MS, APP_LOG_CSV_FORMAT ? "on" : "off");
#endif
    return ESP_OK;
}

void uart_write_line(const char *line, size_t len)
{
#ifdef ARDUINO
    arduino_serial_write(line, len);
#endif
    fwrite(line, 1, len, stdout);
    fflush(stdout);

#if APP_USE_AUX_UART
    uart_write_bytes(UART_NUM_1, line, len);
#endif
}

#endif /* HOST_TEST */
