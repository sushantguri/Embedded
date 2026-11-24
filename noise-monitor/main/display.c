#include "display.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"

#ifndef HOST_TEST
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;
static bool s_oled_ready = false;
static uint8_t s_framebuffer[OLED_FB_SIZE];
#endif

/*
 * Compact 5x7 ASCII bitmap font (codes 0x20 .. 0x7E).
 * Each glyph is 5 bytes (columns left-to-right), LSB at top row.
 */
static const uint8_t FONT5X7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* '}' */
    {0x10, 0x08, 0x08, 0x10, 0x08}, /* '~' */
};

static void fb_clear(uint8_t fb[OLED_FB_SIZE])
{
    memset(fb, 0x00, OLED_FB_SIZE);
}

static void fb_set_pixel(uint8_t fb[OLED_FB_SIZE], int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    size_t idx = (size_t)(y / 8) * OLED_WIDTH + (size_t)x;
    uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) {
        fb[idx] |= mask;
    } else {
        fb[idx] &= (uint8_t)~mask;
    }
}

static void fb_draw_hline(uint8_t fb[OLED_FB_SIZE], int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) {
        fb_set_pixel(fb, x, y, true);
    }
}

static void fb_draw_vline(uint8_t fb[OLED_FB_SIZE], int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++) {
        fb_set_pixel(fb, x, y, true);
    }
}

static void fb_draw_rect(uint8_t fb[OLED_FB_SIZE], int x0, int y0, int x1, int y1)
{
    fb_draw_hline(fb, x0, x1, y0);
    fb_draw_hline(fb, x0, x1, y1);
    fb_draw_vline(fb, x0, y0, y1);
    fb_draw_vline(fb, x1, y0, y1);
}

static void fb_fill_rect(uint8_t fb[OLED_FB_SIZE], int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            fb_set_pixel(fb, x, y, true);
        }
    }
}

static void fb_draw_char(uint8_t fb[OLED_FB_SIZE], int x, int page, char ch)
{
    if (page < 0 || page >= OLED_PAGES) {
        return;
    }
    if (ch < 0x20 || ch > 0x7E) {
        ch = '?';
    }
    const uint8_t *glyph = FONT5X7[(uint8_t)ch - 0x20];
    size_t base = (size_t)page * OLED_WIDTH;
    for (int col = 0; col < 5; col++) {
        int px = x + col;
        if (px >= 0 && px < OLED_WIDTH) {
            fb[base + (size_t)px] = glyph[col];
        }
    }
}

static void fb_draw_text(uint8_t fb[OLED_FB_SIZE], int x, int page, const char *text)
{
    if (text == NULL) {
        return;
    }
    int cur_x = x;
    while (*text != '\0' && cur_x < OLED_WIDTH) {
        fb_draw_char(fb, cur_x, page, *text);
        cur_x += 6;
        text++;
    }
}

void display_format_level_line(char *buf,
                               size_t buf_len,
                               const noise_measurement_t *meas)
{
    if (buf == NULL || buf_len == 0 || meas == NULL) {
        return;
    }

    /*
     * Per specification Section 9:
     * Display dB values ONLY after calibration; otherwise clearly label as
     * relative noise level.
     */
    if (meas->is_calibrated) {
        snprintf(buf, buf_len, "Noise: %5.1f dB", (double)meas->estimated_db);
    } else {
        snprintf(buf, buf_len, "Level: %5.1f%% REL", (double)meas->relative_pct);
    }
}

void display_render_measurement(uint8_t fb[OLED_FB_SIZE],
                                const noise_measurement_t *meas)
{
    if (fb == NULL || meas == NULL) {
        return;
    }

    char line[24];
    fb_clear(fb);

    /* Page 0: Header banner */
    fb_draw_text(fb, 1, 0, "--- NOISE MONITOR ---");

    /* Page 1: Divider + primary noise reading */
    fb_draw_hline(fb, 0, OLED_WIDTH - 1, 9);

    /* Page 2: Level / calibrated dB */
    display_format_level_line(line, sizeof(line), meas);
    fb_draw_text(fb, 2, 2, line);

    /* Page 3: Status indicator */
    if (meas->status == NOISE_STATUS_WARNING) {
        snprintf(line, sizeof(line), "Status: WARNING!");
    } else if (meas->status == NOISE_STATUS_MUTED) {
        snprintf(line, sizeof(line), "Status: MUTED");
    } else {
        snprintf(line, sizeof(line), "Status: NORMAL");
    }
    fb_draw_text(fb, 2, 3, line);

    /* Page 4: Threshold & preset */
    snprintf(line, sizeof(line), "Thr:%4.1f  %.9s",
             (double)meas->threshold_trigger,
             meas->preset_name != NULL ? meas->preset_name : "CUSTOM");
    fb_draw_text(fb, 2, 4, line);

    /* Page 5: RMS diagnostic */
    snprintf(line, sizeof(line), "RMS:%6.1f %s",
             (double)meas->rms_smoothed,
             meas->is_millivolts ? "mV" : "cnt");
    fb_draw_text(fb, 2, 5, line);

    /* Pages 6-7 (y = 50..62): Bar meter with threshold tick mark */
    const int bar_x0 = 2;
    const int bar_x1 = 125;
    const int bar_y0 = 51;
    const int bar_y1 = 61;
    const int inner_w = (bar_x1 - bar_x0) - 2;

    fb_draw_rect(fb, bar_x0, bar_y0, bar_x1, bar_y1);

    float max_scale = meas->is_calibrated ? 110.0f : 100.0f;
    float min_scale = meas->is_calibrated ? 30.0f : 0.0f;
    float span = max_scale - min_scale;

    float norm_val = (meas->primary_value - min_scale) / span;
    if (norm_val < 0.0f) {
        norm_val = 0.0f;
    } else if (norm_val > 1.0f) {
        norm_val = 1.0f;
    }

    int fill_px = (int)(norm_val * (float)inner_w);
    if (fill_px > 0) {
        fb_fill_rect(fb, bar_x0 + 2, bar_y0 + 2, bar_x0 + 1 + fill_px, bar_y1 - 2);
    }

    /* Draw threshold tick mark above and below the bar */
    float norm_thr = (meas->threshold_trigger - min_scale) / span;
    if (norm_thr >= 0.0f && norm_thr <= 1.0f) {
        int thr_x = bar_x0 + 1 + (int)(norm_thr * (float)inner_w);
        fb_draw_vline(fb, thr_x, bar_y0 - 2, bar_y0 - 1);
        fb_draw_vline(fb, thr_x, bar_y1 + 1, bar_y1 + 2);
    }
}

#ifndef HOST_TEST

static esp_err_t ssd1306_send_cmd_list(const uint8_t *cmds, size_t len)
{
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = 0x00; /* Control byte: Co=0, D/C#=0 (command stream) */
    memcpy(&buf[1], cmds, len);
    return i2c_master_transmit(s_oled_dev, buf, len + 1, 200);
}

static esp_err_t ssd1306_flush(app_context_t *ctx, const uint8_t fb[OLED_FB_SIZE])
{
    if (!s_oled_ready || s_oled_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(ctx->i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    static const uint8_t window_cmds[] = {
        0x21, 0x00, 0x7F, /* Column range 0..127 */
        0x22, 0x00, 0x07, /* Page range 0..7 */
    };
    esp_err_t ret = ssd1306_send_cmd_list(window_cmds, sizeof(window_cmds));
    if (ret == ESP_OK) {
        uint8_t page_buf[1 + OLED_WIDTH];
        page_buf[0] = 0x40; /* Control byte: Co=0, D/C#=1 (GDDRAM data) */
        for (int page = 0; page < OLED_PAGES; page++) {
            memcpy(&page_buf[1], &fb[(size_t)page * OLED_WIDTH], OLED_WIDTH);
            ret = i2c_master_transmit(s_oled_dev, page_buf, sizeof(page_buf), 200);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    xSemaphoreGive(ctx->i2c_mutex);
    return ret;
}

esp_err_t display_init(app_context_t *ctx)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)APP_OLED_SDA_GPIO,
        .scl_io_num = (gpio_num_t)APP_OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_OLED_I2C_ADDR,
        .scl_speed_hz = 400000, /* 400 kHz Fast Mode I2C */
    };

    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_oled_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Probe SSD1306 before sending full init sequence */
    ret = i2c_master_probe(s_i2c_bus, APP_OLED_I2C_ADDR, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 OLED not detected at I2C 0x%02X (SDA=%d, SCL=%d); running headless",
                 APP_OLED_I2C_ADDR, APP_OLED_SDA_GPIO, APP_OLED_SCL_GPIO);
        s_oled_ready = false;
        return ESP_ERR_NOT_FOUND;
    }

    static const uint8_t init_cmds[] = {
        0xAE,       /* Display OFF */
        0xD5, 0x80, /* Display clock divide ratio */
        0xA8, 0x3F, /* Multiplex ratio = 64 */
        0xD3, 0x00, /* Display offset = 0 */
        0x40,       /* Start line = 0 */
        0x8D, 0x14, /* Enable internal charge pump */
        0x20, 0x00, /* Horizontal addressing mode */
        0xA1,       /* Segment remap (col 127 -> SEG0) */
        0xC8,       /* COM output scan direction (remapped) */
        0xDA, 0x12, /* COM pins hardware config for 128x64 */
        0x81, 0xCF, /* Contrast control */
        0xD9, 0xF1, /* Pre-charge period */
        0xDB, 0x40, /* VCOMH deselect level */
        0xA4,       /* Resume to RAM content display */
        0xA6,       /* Normal display (non-inverted) */
        0x2E,       /* Deactivate scroll */
        0xAF,       /* Display ON */
    };

    if (xSemaphoreTake(ctx->i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        ret = ssd1306_send_cmd_list(init_cmds, sizeof(init_cmds));
        xSemaphoreGive(ctx->i2c_mutex);
    } else {
        ret = ESP_ERR_TIMEOUT;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init sequence failed: %s", esp_err_to_name(ret));
        s_oled_ready = false;
        return ret;
    }

    s_oled_ready = true;
    ESP_LOGI(TAG, "SSD1306 128x64 OLED ready at 0x%02X (SDA=%d, SCL=%d)",
             APP_OLED_I2C_ADDR, APP_OLED_SDA_GPIO, APP_OLED_SCL_GPIO);
    return ESP_OK;
}

void display_show_splash(app_context_t *ctx)
{
    if (!s_oled_ready) {
        return;
    }

    fb_clear(s_framebuffer);
    fb_draw_text(s_framebuffer, 1, 0, "--- NOISE MONITOR ---");
    fb_draw_hline(s_framebuffer, 0, OLED_WIDTH - 1, 10);
    fb_draw_text(s_framebuffer, 8, 2, "ESP32-S3 + FreeRTOS");
    fb_draw_text(s_framebuffer, 14, 4, "Initializing...");
    fb_draw_text(s_framebuffer, 8, 6,
                 APP_CALIBRATED_MODE_DEFAULT ? "Mode: Calibrated dB" : "Mode: Relative (%)");
    (void)ssd1306_flush(ctx, s_framebuffer);
}

esp_err_t display_update(app_context_t *ctx, const uint8_t fb[OLED_FB_SIZE])
{
    if (!s_oled_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    return ssd1306_flush(ctx, fb);
}

#endif /* HOST_TEST */
