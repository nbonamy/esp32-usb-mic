#include "display.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "power_button.h"
#include "visualizer.h"

#define WIDTH VISUALIZER_WIDTH
#define HEIGHT VISUALIZER_HEIGHT
#define STRIPE_HEIGHT 64
#define VISUAL_TOP 64
#define VISUAL_BOTTOM 384

static const char *TAG = "mic_display";
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t transfer_done;
static uint16_t *dma_stripe;
static display_idle_capture_cb_t idle_capture_cb;
static display_pause_cb_t pause_cb;

void display_record_audio(const int16_t *samples, size_t count)
{
    visualizer_record_audio(samples, count);
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

static esp_err_t draw_rows(int top, int bottom)
{
    for (int y = top; y < bottom; y += STRIPE_HEIGHT) {
        visualizer_draw_stripe(dma_stripe, y, STRIPE_HEIGHT);
        for (int pixel = 0; pixel < WIDTH * STRIPE_HEIGHT; ++pixel) {
            dma_stripe[pixel] = __builtin_bswap16(dma_stripe[pixel]);
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, y, WIDTH,
                                                       y + STRIPE_HEIGHT,
                                                       dma_stripe), TAG, "panel draw");
        ESP_RETURN_ON_FALSE(xSemaphoreTake(transfer_done, pdMS_TO_TICKS(500)) == pdTRUE,
                            ESP_ERR_TIMEOUT, TAG, "panel transfer timeout");
    }
    return ESP_OK;
}

static void display_task(void *context)
{
    (void)context;
    bool paused = false;
    bool was_boot_pressed = false;
    TickType_t last_toggle = 0;
    unsigned mode = 0;
    uint32_t frame_number = 0;
    for (;;) {
        bool boot_pressed = gpio_get_level(GPIO_NUM_0) == 0;
        bool toggle = boot_pressed && !was_boot_pressed;
        was_boot_pressed = boot_pressed;
        TickType_t now = xTaskGetTickCount();
        if (toggle && (TickType_t)(now - last_toggle) > pdMS_TO_TICKS(300)) {
            esp_err_t err = pause_cb(!paused);
            if (err == ESP_OK) {
                paused = !paused;
                last_toggle = now;
                if (paused) {
                    esp_lcd_panel_co5300_set_brightness(panel, 0);
                    esp_lcd_panel_disp_on_off(panel, false);
                } else {
                    esp_lcd_panel_disp_on_off(panel, true);
                    esp_lcd_panel_co5300_set_brightness(panel, 75);
                }
            } else {
                ESP_LOGE(TAG, "pause toggle failed: %s", esp_err_to_name(err));
            }
        }
        bool pwr_pressed = power_button_take_short_press();
        if (!paused) {
            if (pwr_pressed) {
                mode = (mode + 1) % VISUALIZER_MODE_COUNT;
                ESP_LOGI(TAG, "visualization mode %u", mode);
            }
            if (idle_capture_cb) idle_capture_cb();
            visualizer_prepare(mode, frame_number++);
            esp_err_t err = draw_rows(VISUAL_TOP, VISUAL_BOTTOM);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "waveform update stopped: %s", esp_err_to_name(err));
                vTaskDelete(NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

esp_err_t display_show_microphone(display_idle_capture_cb_t idle_capture,
                                  display_pause_cb_t set_paused)
{
    idle_capture_cb = idle_capture;
    pause_cb = set_paused;
    gpio_config_t boot_button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&boot_button), TAG, "BOOT button");
    transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(transfer_done, ESP_ERR_NO_MEM, TAG, "display semaphore");
    dma_stripe = heap_caps_malloc(WIDTH * STRIPE_HEIGHT * sizeof(uint16_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(dma_stripe, ESP_ERR_NO_MEM, TAG, "display stripe");

    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(
        GPIO_NUM_11, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
        WIDTH * STRIPE_HEIGHT * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "display SPI bus");
    const esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(GPIO_NUM_12, color_done, transfer_done);
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_config, &io),
                        TAG, "display panel IO");

    // Waveshare CO5300 V2 initialization sequence.
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
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(io, &panel_config, &panel),
                        TAG, "CO5300 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    // V2 maps visible columns 16 GRAM columns over; without it an edge glows.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, 16, 0), TAG, "V2 panel gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_co5300_set_brightness(panel, 75),
                        TAG, "panel brightness");

    visualizer_init();
    visualizer_prepare(0, 0);
    ESP_RETURN_ON_ERROR(draw_rows(0, HEIGHT), TAG, "initial frame");
    ESP_RETURN_ON_FALSE(xTaskCreate(display_task, "display", 6144, NULL, 2,
                                    NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "waveform task");
    return ESP_OK;
}
