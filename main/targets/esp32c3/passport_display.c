/* AI Passport hint screen.
 *
 * At boot the built-in 240x320 ST7789 panel shows how to reach the web viewer
 * (open the page, connect over USB). This file only adds display support for
 * the FoloToy AI Passport board: the receiver, capture engine, tuning and host
 * protocol are untouched, and a panel failure never stops the SDR.
 *
 * Panel wiring and the ST7789P3 vendor init sequence follow the FoloToy BSP
 * reference driver for this board. */
#include "passport_display.h"

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "passport_font8x8.h"

#define HINT_LCD_W 240
#define HINT_LCD_H 320
#define HINT_LCD_HOST SPI2_HOST
#define HINT_LCD_MOSI 9
#define HINT_LCD_SCLK 8
#define HINT_LCD_CS 1
#define HINT_LCD_DC 20
#define HINT_LCD_RST (-1)   /* not wired to the MCU: esp_lcd uses SWRESET */
#define HINT_LCD_BL 21      /* backlight, LEDC PWM */
#define HINT_LCD_PCLK_HZ (80 * 1000 * 1000)
#define HINT_LCD_SPI_MODE 0 /* ST7789: idle low, sample on rising edge */

#define HINT_BL_LEDC_TIMER LEDC_TIMER_0
#define HINT_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define HINT_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define HINT_BL_LEDC_RES LEDC_TIMER_10_BIT
#define HINT_BL_LEDC_FREQ_HZ 5000

#define HINT_LINE_PX 16 /* line buffer height: the largest text scale */

static const char *TAG = "passport_display";

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t len;
    uint16_t delay_ms;
} hint_lcd_cmd_t;

/* ST7789P3 vendor specific init sequence (porch / power / gamma): the panel
 * vendor's reference values, not generic ST7789 defaults. */
static const hint_lcd_cmd_t ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0}, /* PORCTRL */
    {0xB7, {0x35}, 1, 0},                         /* GCTRL */
    {0xBB, {0x21}, 1, 0},                         /* VCOMS */
    {0xC0, {0x2C}, 1, 0},                         /* LCMCTRL */
    {0xC2, {0x01}, 1, 0},                         /* VDVVRHEN */
    {0xC3, {0x0B}, 1, 0},                         /* VRHS */
    {0xC4, {0x20}, 1, 0},                         /* VDVSET */
    {0xC6, {0x0F}, 1, 0},                         /* FRCTRL2 */
    {0xD0, {0xA7, 0xA1}, 2, 0},                   /* PWCTRL1 */
    {0xD0, {0xA4, 0xA1}, 2, 0},                   /* PWCTRL1 (reference resend) */
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0}, /* PVGAMCTRL */
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10}, /* NVGAMCTRL */
};

/* The panel expects big-endian 16-bit RGB565 on the wire. */
static uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t v = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

static void draw_text(uint16_t *buf, int width, int buf_h, int x0,
                      const char *text, int scale, uint16_t color) {
    int x = x0;
    for (const char *p = text; *p != '\0'; p++) {
        const unsigned ch = (unsigned char)*p;
        if (ch < 0x20u || ch > 0x7Fu) {
            continue;
        }
        const uint8_t *glyph = PASSPORT_FONT8X8[ch - 0x20u];
        for (int gx = 0; gx < 8; gx++) {
            for (int gy = 0; gy < 8; gy++) {
                if ((glyph[gx] & (1u << gy)) == 0) {
                    continue;
                }
                for (int sy = 0; sy < scale; sy++) {
                    const int py = gy * scale + sy;
                    if (py < 0 || py >= buf_h) {
                        continue;
                    }
                    for (int sx = 0; sx < scale; sx++) {
                        const int px = x + gx * scale + sx;
                        if (px < 0 || px >= width) {
                            continue;
                        }
                        buf[py * width + px] = color;
                    }
                }
            }
        }
        x += 8 * scale;
    }
}

typedef struct {
    const char *text;
    int y;
    int scale;
    uint16_t color;
} hint_line_t;

static void backlight_on(void) {
    ledc_timer_config_t timer = {
        .speed_mode = HINT_BL_LEDC_MODE,
        .duty_resolution = HINT_BL_LEDC_RES,
        .timer_num = HINT_BL_LEDC_TIMER,
        .freq_hz = HINT_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        return;
    }
    ledc_channel_config_t ch = {
        .gpio_num = HINT_LCD_BL,
        .speed_mode = HINT_BL_LEDC_MODE,
        .channel = HINT_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HINT_BL_LEDC_TIMER,
        .duty = (1u << 10) - 1u,
        .hpoint = 0,
    };
    (void)ledc_channel_config(&ch);
}

void passport_display_show_hint(void) {
    static const hint_line_t lines[] = {
        {"RADIO SPECTRUM", 30, 2, 0},   /* color filled in below */
        {"AI Passport", 62, 1, 0},
        {"Open a browser", 110, 2, 0},
        {"espargos.net", 142, 2, 0},
        {"/espsdr/app/", 172, 2, 0},
        {"Click CONNECT", 206, 2, 0},
        {"Connect the device to a PC", 250, 1, 0},
        {"with a USB cable", 266, 1, 0},
    };
    const int line_count = (int)(sizeof(lines) / sizeof(lines[0]));
    uint16_t line_colors[8];
    for (int i = 0; i < line_count; i++) {
        line_colors[i] = rgb565_be(255, 255, 255);
    }
    line_colors[0] = rgb565_be(255, 210, 40);  /* title: amber */
    line_colors[1] = rgb565_be(120, 120, 120); /* subtitle: gray */
    line_colors[3] = rgb565_be(60, 230, 120);  /* url: green */
    line_colors[4] = rgb565_be(60, 230, 120);
    line_colors[6] = rgb565_be(120, 120, 120);
    line_colors[7] = rgb565_be(120, 120, 120);

    spi_bus_config_t bus = {
        .mosi_io_num = HINT_LCD_MOSI,
        .sclk_io_num = HINT_LCD_SCLK,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = HINT_LCD_W * HINT_LINE_PX * 2,
    };
    esp_err_t e = spi_bus_initialize(HINT_LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(e));
        return;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = HINT_LCD_CS,
        .dc_gpio_num = HINT_LCD_DC,
        .pclk_hz = HINT_LCD_PCLK_HZ,
        .spi_mode = HINT_LCD_SPI_MODE,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)HINT_LCD_HOST, &io_cfg, &io);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "panel io init failed: %s", esp_err_to_name(e));
        return;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = HINT_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(io, &dev, &panel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(e));
        return;
    }
    if (esp_lcd_panel_reset(panel) != ESP_OK || esp_lcd_panel_init(panel) != ESP_OK) {
        ESP_LOGE(TAG, "panel reset/init failed");
        return;
    }
    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const hint_lcd_cmd_t *c = &ST7789P3_CMDS[i];
        if (esp_lcd_panel_io_tx_param(io, c->cmd, c->data, c->len) != ESP_OK) {
            ESP_LOGW(TAG, "vendor cmd 0x%02X failed", c->cmd);
        }
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
    (void)esp_lcd_panel_invert_color(panel, true);
    (void)esp_lcd_panel_mirror(panel, false, false);
    (void)esp_lcd_panel_set_gap(panel, 0, 0);
    (void)esp_lcd_panel_disp_on_off(panel, true);
    backlight_on();

    uint16_t *line = heap_caps_malloc(HINT_LCD_W * HINT_LINE_PX * 2,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!line) {
        ESP_LOGE(TAG, "no memory for the hint line buffer");
        return;
    }

    /* Clear the screen. */
    memset(line, 0, HINT_LCD_W * HINT_LINE_PX * 2);
    for (int y = 0; y < HINT_LCD_H; y += HINT_LINE_PX) {
        (void)esp_lcd_panel_draw_bitmap(panel, 0, y, HINT_LCD_W, y + HINT_LINE_PX, line);
    }

    /* Draw the message, one text line at a time. */
    for (int i = 0; i < line_count; i++) {
        const hint_line_t *l = &lines[i];
        const int h = 8 * l->scale;
        const int len = (int)strlen(l->text);
        int x0 = (HINT_LCD_W - len * 8 * l->scale) / 2;
        if (x0 < 0) {
            x0 = 0;
        }
        memset(line, 0, HINT_LCD_W * HINT_LINE_PX * 2);
        draw_text(line, HINT_LCD_W, HINT_LINE_PX, x0, l->text, l->scale, line_colors[i]);
        (void)esp_lcd_panel_draw_bitmap(panel, 0, l->y, HINT_LCD_W, l->y + h, line);
    }
    heap_caps_free(line);
    ESP_LOGI(TAG, "hint screen drawn");
}
