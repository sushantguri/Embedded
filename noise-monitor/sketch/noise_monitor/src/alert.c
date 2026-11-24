#include "alert.h"

#ifndef HOST_TEST
#include "driver/gpio.h"
#if APP_BUZZER_USE_PWM
#include "driver/ledc.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "processing.h"

static const char *TAG = "alert";
static volatile int64_t s_last_btn_isr_us = 0;

static void IRAM_ATTR button_isr_handler(void *arg)
{
    app_context_t *ctx = (app_context_t *)arg;
    int64_t now_us = esp_timer_get_time();

    if ((now_us - s_last_btn_isr_us) < ((int64_t)APP_BUTTON_DEBOUNCE_MS * 1000LL)) {
        return;
    }
    s_last_btn_isr_us = now_us;

    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->button_sem, &higher_prio_woken);
    portYIELD_FROM_ISR(higher_prio_woken);
}

static void buzzer_set(bool on)
{
#if APP_BUZZER_USE_PWM
    uint32_t duty = on ? 512U : 0U; /* 50% duty on 10-bit resolution */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    gpio_set_level((gpio_num_t)APP_BUZZER_GPIO, on ? 1 : 0);
#endif
}

esp_err_t alert_init(app_context_t *ctx)
{
    /* Configure Green and Red status LEDs */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << APP_GREEN_LED_GPIO) | (1ULL << APP_RED_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&led_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Configure Buzzer (LEDC PWM for passive buzzer or digital GPIO for active buzzer) */
#if APP_BUZZER_USE_PWM
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = APP_BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = APP_BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
#else
    gpio_config_t buz_cfg = {
        .pin_bit_mask = (1ULL << APP_BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&buz_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
#endif

    /* Configure Push Button with falling-edge interrupt and internal pull-up */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << APP_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_isr_handler_add((gpio_num_t)APP_BUTTON_GPIO, button_isr_handler, ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initial safe state: Green LED ON, Red LED OFF, Buzzer OFF */
    alert_apply_state(NOISE_STATUS_NORMAL, false);

    ESP_LOGI(TAG, "Alerts initialized: Green=%d, Red=%d, Buzzer=%d (%s), Button=%d",
             APP_GREEN_LED_GPIO, APP_RED_LED_GPIO, APP_BUZZER_GPIO,
             APP_BUZZER_USE_PWM ? "PWM" : "GPIO", APP_BUTTON_GPIO);
    return ESP_OK;
}

void alert_apply_state(noise_status_t status, bool pulse_on)
{
    switch (status) {
        case NOISE_STATUS_NORMAL:
            gpio_set_level((gpio_num_t)APP_GREEN_LED_GPIO, 1);
            gpio_set_level((gpio_num_t)APP_RED_LED_GPIO, 0);
            buzzer_set(false);
            break;

        case NOISE_STATUS_WARNING:
            gpio_set_level((gpio_num_t)APP_GREEN_LED_GPIO, 0);
            gpio_set_level((gpio_num_t)APP_RED_LED_GPIO, 1);
            buzzer_set(pulse_on);
            break;

        case NOISE_STATUS_MUTED:
            gpio_set_level((gpio_num_t)APP_GREEN_LED_GPIO, 0);
            gpio_set_level((gpio_num_t)APP_RED_LED_GPIO, 1);
            buzzer_set(false);
            break;

        default:
            gpio_set_level((gpio_num_t)APP_GREEN_LED_GPIO, 0);
            gpio_set_level((gpio_num_t)APP_RED_LED_GPIO, 0);
            buzzer_set(false);
            break;
    }
}

void alert_handle_button_press(app_context_t *ctx, const noise_measurement_t *latest)
{
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    if (latest->alert_active) {
        /* Acknowledge / toggle audible alarm mute */
        ctx->config.muted = !ctx->config.muted;
        ESP_LOGI(TAG, "Button press: alarm %s", ctx->config.muted ? "MUTED" : "UNMUTED");
    } else {
        /* Cycle through monitoring environment threshold presets */
        size_t preset_count = 0;
        const noise_threshold_preset_t *presets = processing_get_presets(&preset_count);
        if (preset_count > 0) {
            ctx->config.preset_index = (ctx->config.preset_index + 1) % preset_count;
            ctx->config.threshold_trigger = presets[ctx->config.preset_index].trigger_threshold;
            ctx->config.threshold_reset = presets[ctx->config.preset_index].reset_threshold;
            ESP_LOGI(TAG, "Button press: switched preset -> %s (trigger=%.1f, reset=%.1f)",
                     presets[ctx->config.preset_index].name,
                     (double)ctx->config.threshold_trigger,
                     (double)ctx->config.threshold_reset);
        }
    }

    xSemaphoreGive(ctx->state_mutex);
}

#endif /* HOST_TEST */
