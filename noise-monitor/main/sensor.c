#include "sensor.h"

#include <string.h>

#ifndef HOST_TEST
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"

#if APP_MIC_BACKEND_ADC
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h"
#else
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#endif

static const char *TAG = "sensor";

#if APP_MIC_BACKEND_ADC

/* Compatibility helper across ESP-IDF v5.0 - v6.x */
#ifdef ADC_ATTEN_DB_12
#define APP_ADC_ATTEN ADC_ATTEN_DB_12
#else
#define APP_ADC_ATTEN ADC_ATTEN_DB_11
#endif

#define ADC_FRAME_BYTES (APP_AUDIO_BLOCK_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)
#define ADC_POOL_BYTES  (ADC_FRAME_BYTES * 4)

static adc_continuous_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_cali_enabled = false;
static adc_unit_t s_adc_unit = ADC_UNIT_1;
static adc_channel_t s_adc_channel = ADC_CHANNEL_3;
static uint8_t s_dma_buf[ADC_FRAME_BYTES];

static bool init_adc_calibration(adc_unit_t unit, adc_channel_t channel)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = APP_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC curve-fitting calibration initialized (unit=%d, ch=%d)",
                 unit, channel);
        return true;
    }
    ESP_LOGW(TAG, "ADC curve-fitting calibration unavailable (%s); using nominal scaling",
             esp_err_to_name(ret));
#else
    (void)unit;
    (void)channel;
    ESP_LOGW(TAG, "ADC curve-fitting scheme not supported on this target; using nominal scaling");
#endif
    return false;
}

esp_err_t sensor_init(void)
{
    esp_err_t ret = adc_continuous_io_to_channel(APP_MIC_ADC_GPIO, &s_adc_unit, &s_adc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not a valid ADC pin (%s)", APP_MIC_ADC_GPIO, esp_err_to_name(ret));
        return ret;
    }
    if (s_adc_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO %d maps to ADC2; please select an ADC1 GPIO (GPIO 1..10 on ESP32-S3)",
                 APP_MIC_ADC_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = ADC_POOL_BYTES,
        .conv_frame_size = ADC_FRAME_BYTES,
        .flags = {
            .flush_pool = 1,
        },
    };
    ret = adc_continuous_new_handle(&handle_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_digi_pattern_config_t pattern = {
        .atten = APP_ADC_ATTEN,
        .channel = (uint8_t)s_adc_channel,
        .unit = (uint8_t)s_adc_unit,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };

    adc_continuous_config_t dig_cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = APP_SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

    ret = adc_continuous_config(s_adc_handle, &dig_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config failed: %s", esp_err_to_name(ret));
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return ret;
    }

    s_cali_enabled = init_adc_calibration(s_adc_unit, s_adc_channel);

    ret = adc_continuous_start(s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start failed: %s", esp_err_to_name(ret));
        sensor_deinit();
        return ret;
    }

    ESP_LOGI(TAG, "Analog microphone sensor ready: GPIO %d (ADC1_CH%d), %d Hz, %d samples/block",
             APP_MIC_ADC_GPIO, s_adc_channel, APP_SAMPLE_RATE_HZ, APP_AUDIO_BLOCK_SAMPLES);
    return ESP_OK;
}

esp_err_t sensor_read_block(audio_block_t *out_block, uint32_t timeout_ms)
{
    if (s_adc_handle == NULL || out_block == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t collected = 0;
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    while (collected < APP_AUDIO_BLOCK_SAMPLES) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        uint32_t wait_ms = (uint32_t)(remaining_us / 1000LL);
        if (wait_ms == 0) {
            wait_ms = 1;
        }

        uint32_t bytes_read = 0;
        esp_err_t ret = adc_continuous_read(s_adc_handle, s_dma_buf, sizeof(s_dma_buf),
                                            &bytes_read, wait_ms);
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (ret != ESP_OK) {
            return ret;
        }

        for (uint32_t i = 0; i < bytes_read; i += SOC_ADC_DIGI_RESULT_BYTES) {
            if (collected >= APP_AUDIO_BLOCK_SAMPLES) {
                break;
            }
            adc_digi_output_data_t *p = (adc_digi_output_data_t *)&s_dma_buf[i];
            uint32_t raw_val = p->type2.data;

            if (s_cali_enabled && s_cali_handle != NULL) {
                int voltage_mv = 0;
                if (adc_cali_raw_to_voltage(s_cali_handle, (int)raw_val, &voltage_mv) == ESP_OK) {
                    out_block->samples[collected++] = (int32_t)voltage_mv;
                    continue;
                }
            }

            /* Nominal 12-bit mapping (3300 mV / 4095 counts) */
            out_block->samples[collected++] = (int32_t)((raw_val * 3300UL) / 4095UL);
        }
    }

    out_block->count = collected;
    out_block->is_millivolts = true;
    return ESP_OK;
}

void sensor_deinit(void)
{
    if (s_adc_handle != NULL) {
        adc_continuous_stop(s_adc_handle);
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (s_cali_handle != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_cali_handle);
        s_cali_handle = NULL;
    }
#endif
    s_cali_enabled = false;
}

#else /* APP_MIC_BACKEND_ADC == 0 (I2S Digital Microphone backend) */

static i2s_chan_handle_t s_rx_handle = NULL;

esp_err_t sensor_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(APP_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)APP_MIC_I2S_BCLK_GPIO,
            .ws   = (gpio_num_t)APP_MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)APP_MIC_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Digital I2S sensor ready: BCLK=%d, WS=%d, DIN=%d, %d Hz",
             APP_MIC_I2S_BCLK_GPIO, APP_MIC_I2S_WS_GPIO, APP_MIC_I2S_DIN_GPIO, APP_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t sensor_read_block(audio_block_t *out_block, uint32_t timeout_ms)
{
    if (s_rx_handle == NULL || out_block == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    static int32_t raw_i2s_buf[APP_AUDIO_BLOCK_SAMPLES];
    size_t bytes_read = 0;

    esp_err_t ret = i2s_channel_read(s_rx_handle, raw_i2s_buf,
                                     sizeof(raw_i2s_buf), &bytes_read,
                                     timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t samples_read = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < samples_read; i++) {
        /* INMP441 outputs 24-bit audio aligned into 32-bit word; shift down */
        out_block->samples[i] = raw_i2s_buf[i] >> 8;
    }

    out_block->count = samples_read;
    out_block->is_millivolts = false;
    return ESP_OK;
}

void sensor_deinit(void)
{
    if (s_rx_handle != NULL) {
        i2s_channel_disable(s_rx_handle);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }
}

#endif /* APP_MIC_BACKEND_ADC */

#endif /* HOST_TEST */
