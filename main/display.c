#include "display.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WIDTH 368
#define HEIGHT 448
#define STRIPE_HEIGHT 16

static const char *TAG = "mic_display";

// Minimal 5x7 uppercase font for the fixed status screen. Each byte is a row.
typedef struct { char letter; uint8_t row[7]; } glyph_t;
static const glyph_t glyphs[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'K', {17, 18, 20, 24, 20, 18, 17}},
    {'M', {17, 27, 21, 21, 17, 17, 17}},
    {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
    {'Z', {31, 1, 2, 4, 8, 16, 31}},
};

static bool text_pixel(const char *text, int left, int top, int scale, int x, int y)
{
    int px = x - left;
    int py = y - top;
    if (px < 0 || py < 0 || py >= 7 * scale) return false;
    int character = px / (6 * scale);
    if (character >= (int)strlen(text)) return false;
    int column = (px / scale) % 6;
    if (column >= 5) return false;
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
        if (glyphs[i].letter == text[character]) {
            return (glyphs[i].row[py / scale] & (1 << (4 - column))) != 0;
        }
    }
    return false;
}

static uint16_t pixel(int x, int y)
{
    const uint16_t background = 0x0845;
    const uint16_t muted = 0x6B94;
    const uint16_t teal = 0x36DA;
    const uint16_t white = 0xF7FE;
    const uint16_t green = 0x5F2C;
    uint16_t color = background;

    // Subtle framed field and an accent rule.
    if (x >= 20 && x < 348 && y >= 18 && y < 430 &&
        (x < 22 || x >= 346 || y < 20 || y >= 428)) color = 0x1949;
    if (y >= 79 && y < 82 && x >= 46 && x < 322) color = teal;

    int dx = x - 184;
    int dy = y - 189;
    int distance = dx * dx + dy * dy;
    if (distance >= 91 * 91 && distance <= 94 * 94) color = 0x21D2;

    // Mic capsule, pickup arc, stem, and foot.
    if ((x >= 166 && x <= 202 && y >= 147 && y <= 205) ||
        (dx * dx + (y - 147) * (y - 147) <= 18 * 18 && y < 147) ||
        (dx * dx + (y - 205) * (y - 205) <= 18 * 18 && y > 205)) color = teal;
    int arc = dx * dx + (y - 191) * (y - 191);
    if (y >= 185 && y <= 240 && arc >= 48 * 48 && arc <= 52 * 52) color = white;
    if (x >= 181 && x <= 187 && y >= 237 && y <= 253) color = white;
    if (x >= 159 && x <= 209 && y >= 251 && y <= 256) color = white;

    if (text_pixel("WAVESHARE", 76, 40, 4, x, y)) color = muted;
    if (text_pixel("USB MIC", 58, 304, 6, x, y)) color = white;
    if (text_pixel("24 KHZ MONO", 118, 367, 2, x, y)) color = muted;
    if (x >= 128 && x < 240 && y >= 402 && y < 428 &&
        (x < 130 || x >= 238 || y < 404 || y >= 426)) color = green;
    if (text_pixel("READY", 154, 408, 2, x, y)) color = green;

    // QSPI panel expects the most significant RGB565 byte first.
    return __builtin_bswap16(color);
}

static bool IRAM_ATTR color_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *event,
                                 void *context)
{
    (void)io;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

esp_err_t display_show_microphone(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(done, ESP_ERR_NO_MEM, TAG, "display semaphore");
    uint16_t *stripe = heap_caps_malloc(WIDTH * STRIPE_HEIGHT * sizeof(uint16_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(stripe, ESP_ERR_NO_MEM, TAG, "display stripe");

    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(
        GPIO_NUM_11, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
        WIDTH * STRIPE_HEIGHT * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "display SPI bus");
    const esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(GPIO_NUM_12, color_done, done);
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_config, &io),
                        TAG, "display panel IO");

    // Exact CO5300 V2 sequence from Waveshare's ESP32-S3-Touch-AMOLED-1.8 BSP.
    const co5300_lcd_init_cmd_t commands[] = {
        {0xFE, (uint8_t[]){0x00}, 1, 0},
        {0xC4, (uint8_t[]){0x80}, 1, 0},
        {0x3A, (uint8_t[]){0x55}, 1, 0},
        {0x35, (uint8_t[]){0x00}, 1, 0},
        {0x53, (uint8_t[]){0x20}, 1, 0},
        {0x51, (uint8_t[]){0xFF}, 1, 0},
        {0x63, (uint8_t[]){0xFF}, 1, 0},
        {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
        {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
        {0x11, (uint8_t[]){0x00}, 0, 100},
        {0x29, (uint8_t[]){0x00}, 0, 0},
    };
    co5300_vendor_config_t vendor = {
        .init_cmds = commands,
        .init_cmds_size = sizeof(commands) / sizeof(commands[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_config, &panel),
                        TAG, "CO5300 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    // Waveshare's V2 panel maps its visible 368 columns 16 GRAM columns over.
    // Without this offset, the untouched edge appears as a bright vertical bar.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, 16, 0), TAG, "V2 panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(panel, 75),
                        TAG, "panel brightness");

    for (int y = 0; y < HEIGHT; y += STRIPE_HEIGHT) {
        for (int row = 0; row < STRIPE_HEIGHT; ++row) {
            for (int x = 0; x < WIDTH; ++x) {
                stripe[row * WIDTH + x] = pixel(x, y + row);
            }
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, y, WIDTH,
                                                       y + STRIPE_HEIGHT, stripe),
                            TAG, "panel draw");
        ESP_RETURN_ON_FALSE(xSemaphoreTake(done, pdMS_TO_TICKS(500)) == pdTRUE,
                            ESP_ERR_TIMEOUT, TAG, "panel transfer timeout");
    }
    free(stripe);
    vSemaphoreDelete(done);
    return ESP_OK;
}
