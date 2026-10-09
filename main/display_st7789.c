#include "display_st7789.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "LCD";

#define LCD_FLUSH_ROWS 8
#define LCD_SPI_CLOCK_HZ (20 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_framebuffer;
static SemaphoreHandle_t s_mutex;

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
}

static void clear(uint16_t color)
{
    for (int index = 0; index < APP_LCD_H_RES * APP_LCD_V_RES; ++index) {
        s_framebuffer[index] = color;
    }
}

static void fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > APP_LCD_H_RES) width = APP_LCD_H_RES - x;
    if (y + height > APP_LCD_V_RES) height = APP_LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            s_framebuffer[row * APP_LCD_H_RES + column] = color;
        }
    }
}

static void draw_char(int x, int y, char character, int scale, uint16_t color)
{
    const uint8_t *rows = font5x7_rows(character);
    for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
            if (rows[row] & (1U << (4 - column))) {
                fill_rect(x + column * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale, uint16_t color)
{
    if (!text) return;
    for (size_t index = 0; text[index] != '\0'; ++index) {
        draw_char(x, y, text[index], scale, color);
        x += 6 * scale;
        if (x > APP_LCD_H_RES - 6 * scale) break;
    }
}

static void flush(void)
{
    /*
     * The full 240x240 RGB565 framebuffer is 115200 bytes and lives in PSRAM.
     * SPI DMA cannot transmit that PSRAM buffer directly, so the driver would
     * try to allocate one equally large internal DMA bounce buffer. WakeNet,
     * audio and FFT leave too little contiguous internal RAM for that request.
     * Send a few rows per transaction instead; the driver's temporary DMA
     * buffer is then only 3840 bytes (8 rows), avoiding allocation failures.
     */
    for (int y = 0; y < APP_LCD_V_RES; y += LCD_FLUSH_ROWS) {
        int y_end = y + LCD_FLUSH_ROWS;
        if (y_end > APP_LCD_V_RES) {
            y_end = APP_LCD_V_RES;
        }

        esp_err_t error = esp_lcd_panel_draw_bitmap(
            s_panel,
            0,
            y,
            APP_LCD_H_RES,
            y_end,
            &s_framebuffer[y * APP_LCD_H_RES]
        );
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "draw rows %d..%d failed: %s", y, y_end, esp_err_to_name(error));
            break;
        }
    }
}

esp_err_t display_st7789_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, "LCD", "mutex allocation failed");


    size_t framebuffer_size =
        APP_LCD_H_RES * APP_LCD_V_RES * sizeof(uint16_t);


    ESP_LOGI(TAG,
             "Framebuffer size: %d bytes",
             framebuffer_size);


    ESP_LOGI(TAG,
             "PSRAM total=%d free=%d",
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));


    /*
     * First try PSRAM
     */
    s_framebuffer = heap_caps_calloc(
        1,
        framebuffer_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );


    if (s_framebuffer != NULL) {

        ESP_LOGI(TAG,
                 "Framebuffer allocated in PSRAM");

    }
    else {

        ESP_LOGW(TAG,
                 "PSRAM allocation failed, trying internal RAM");


        /*
         * Fallback to internal RAM
         */
        s_framebuffer = heap_caps_calloc(
            1,
            framebuffer_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );


        if (s_framebuffer != NULL) {

            ESP_LOGI(TAG,
                     "Framebuffer allocated in internal RAM");

        }
    }


    ESP_RETURN_ON_FALSE(
        s_framebuffer,
        ESP_ERR_NO_MEM,
        "LCD",
        "framebuffer allocation failed"
    );

    spi_bus_config_t bus_config = {
        .sclk_io_num = APP_LCD_SCLK_GPIO,
        .mosi_io_num = APP_LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = APP_LCD_H_RES * LCD_FLUSH_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO), "LCD", "SPI bus init failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = APP_LCD_DC_GPIO,
        .cs_gpio_num = APP_LCD_CS_GPIO,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle),
        "LCD",
        "panel IO init failed"
    );

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = APP_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel), "LCD", "ST7789 init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), "LCD", "reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), "LCD", "panel init failed");
    esp_lcd_panel_set_gap(s_panel, APP_LCD_X_GAP, APP_LCD_Y_GAP);
    esp_lcd_panel_invert_color(s_panel, APP_LCD_INVERT_COLOR);
    esp_lcd_panel_swap_xy(s_panel, APP_LCD_SWAP_XY);
    esp_lcd_panel_mirror(s_panel, APP_LCD_MIRROR_X, APP_LCD_MIRROR_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);
    display_show_message("VIOLIN", "BOOTING", "PLEASE WAIT");
    return ESP_OK;
}

void display_show_home(bool wifi_connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t white = rgb565(255,255,255), cyan = rgb565(40,220,255), green = rgb565(60,255,80);
    clear(rgb565(0,0,18));
    draw_text(30, 24, "VIOLIN LITE", 3, cyan);
    draw_text(36, 82, wifi_connected ? "WIFI READY" : "WIFI OFF", 2, wifi_connected ? green : rgb565(255,100,60));
    draw_text(24, 126, "SAY: NI HAO XIAO ZHI", 2, white);
    draw_text(36, 166, "SAY: PRACTICE A4", 2, white);
    flush();
    xSemaphoreGive(s_mutex);
}

void display_show_practice(
    const char *target_note,
    float target_hz,
    bool valid,
    float measured_hz,
    float cents,
    float confidence,
    const char *progress
)
{
    char line[48];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clear(rgb565(0,0,12));
    uint16_t white = rgb565(245,245,245), yellow = rgb565(255,220,40), cyan = rgb565(40,220,255);
    uint16_t status_color = rgb565(255,120,60);
    const char *status = "LISTENING";
    if (valid) {
        if (fabsf(cents) <= APP_CORRECT_CENTS) { status = "IN TUNE"; status_color = rgb565(40,255,80); }
        else if (cents > 0.0f) { status = "HIGH"; status_color = rgb565(255,90,60); }
        else { status = "LOW"; status_color = rgb565(255,190,40); }
    }

    /*
     * 连续两个相同音时，目标音名字不会变化。
     * 明确告诉用户需要停弓/重新起音，避免看起来像“卡住”。
     */
    if (
        progress
        && strstr(progress, "RELEASE")
    ) {
        status = "RELEASE BOW";
        status_color = rgb565(255,220,40);
    }

    snprintf(line, sizeof(line), "TARGET %s", target_note ? target_note : "--");
    draw_text(18, 14, line, 3, cyan);
    snprintf(line, sizeof(line), "STD %.2F HZ", target_hz);
    draw_text(18, 52, line, 2, white);
    if (valid) {
        snprintf(line, sizeof(line), "FREQ %.2F HZ", measured_hz);
        draw_text(18, 88, line, 2, yellow);
        snprintf(line, sizeof(line), "CENTS %+.1F", cents);
        draw_text(18, 120, line, 2, white);
        snprintf(line, sizeof(line), "CONF %.0F%%", confidence * 100.0f);
        draw_text(18, 152, line, 2, white);
    } else {
        draw_text(36, 102, "WAIT FOR SOUND", 2, white);
    }
    draw_text(54, 188, status, 3, status_color);
    if (progress && progress[0]) draw_text(12, 220, progress, 1, white);
    flush();
    xSemaphoreGive(s_mutex);
}

void display_show_message(const char *title, const char *line1, const char *line2)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clear(rgb565(0,0,16));
    draw_text(24, 34, title ? title : "MESSAGE", 3, rgb565(40,220,255));
    draw_text(18, 104, line1 ? line1 : "", 2, rgb565(255,255,255));
    draw_text(18, 148, line2 ? line2 : "", 2, rgb565(255,220,40));
    flush();
    xSemaphoreGive(s_mutex);
}
