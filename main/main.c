#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_device_uac.h"

#include "codec/es8311.h"
#include "display.h"
#include "serial_control.h"

#define SAMPLE_RATE 24000
#define FRAMES_PER_READ (SAMPLE_RATE / 100)  // UAC asks for 10 ms.
#define I2C_PORT I2C_NUM_0
#define CODEC_ADDRESS ES8311_ADDRESS_0

static const char *TAG = "usb_mic";
static i2s_chan_handle_t rx_channel;
static int16_t stereo[FRAMES_PER_READ * 2];
static uint32_t read_errors;
static volatile TickType_t last_usb_capture_tick;

static esp_err_t microphone_input(uint8_t *buffer, size_t length,
                                  size_t *bytes_read, void *context)
{
    (void)context;
    *bytes_read = 0;
    if (length > FRAMES_PER_READ * sizeof(int16_t) || length % sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    last_usb_capture_tick = xTaskGetTickCount();

    const size_t frames = length / sizeof(int16_t);
    size_t received = 0;
    esp_err_t err = i2s_channel_read(rx_channel, stereo, frames * 2 * sizeof(int16_t),
                                     &received, pdMS_TO_TICKS(20));
    if (err != ESP_OK || received != frames * 2 * sizeof(int16_t)) {
        memset(buffer, 0, length);
        *bytes_read = length;
        display_record_peak(0);
        if (++read_errors % 100 == 1) {
            ESP_LOGW(TAG, "I2S short read: %s, %u/%u bytes",
                     esp_err_to_name(err), (unsigned)received,
                     (unsigned)(frames * 2 * sizeof(int16_t)));
        }
        return ESP_OK;
    }

    // ES8311 capture presents stereo slots. The physical mic appears in the
    // stronger slot; follow the known-good Codex Remote channel selection.
    uint32_t energy[2] = {0, 0};
    for (size_t i = 0; i < frames; ++i) {
        energy[0] += abs((int)stereo[2 * i]);
        energy[1] += abs((int)stereo[2 * i + 1]);
    }
    const unsigned channel = energy[1] > energy[0] ? 1 : 0;
    int16_t *mono = (int16_t *)buffer;
    uint16_t peak = 0;
    for (size_t i = 0; i < frames; ++i) {
        mono[i] = stereo[2 * i + channel];
        unsigned magnitude = abs((int)mono[i]);
        if (magnitude > peak) peak = magnitude;
    }
    display_record_peak(peak);
    *bytes_read = length;
    return ESP_OK;
}

static void capture_idle_peak(void)
{
    // Keep the display reactive even when macOS has not opened the UAC stream.
    // USB capture has priority and owns I2S while it is active.
    if ((TickType_t)(xTaskGetTickCount() - last_usb_capture_tick) <
        pdMS_TO_TICKS(100)) return;

    int16_t samples[FRAMES_PER_READ * 2];
    size_t received = 0;
    esp_err_t err = i2s_channel_read(rx_channel, samples, sizeof(samples),
                                     &received, pdMS_TO_TICKS(20));
    if (err != ESP_OK || received != sizeof(samples)) return;

    uint32_t energy[2] = {0, 0};
    uint16_t peak[2] = {0, 0};
    for (size_t i = 0; i < FRAMES_PER_READ; ++i) {
        for (unsigned channel = 0; channel < 2; ++channel) {
            unsigned magnitude = abs((int)samples[2 * i + channel]);
            energy[channel] += magnitude;
            if (magnitude > peak[channel]) peak[channel] = magnitude;
        }
    }
    display_record_peak(peak[energy[1] > energy[0] ? 1 : 0]);
}

static esp_err_t init_audio(void)
{
    // GPIO46 enables the speaker amplifier. Keep it off for an input-only device.
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << GPIO_NUM_46,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "amp GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_46, 0), TAG, "amp off");

    i2c_config_t i2c = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_15,
        .scl_io_num = GPIO_NUM_14,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &i2c), TAG, "I2C pins");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, i2c.mode, 0, 0, 0),
                        TAG, "I2C driver");

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 4;
    channel_config.dma_frame_num = FRAMES_PER_READ;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &rx_channel),
                        TAG, "I2S RX channel");
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_16,
            .bclk = GPIO_NUM_9,
            .ws = GPIO_NUM_45,
            .dout = I2S_GPIO_UNUSED,
            .din = GPIO_NUM_10,
        },
    };
    standard.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_channel, &standard),
                        TAG, "I2S standard mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_channel), TAG, "I2S enable");

    es8311_handle_t codec = es8311_create(I2C_PORT, CODEC_ADDRESS);
    ESP_RETURN_ON_FALSE(codec, ESP_ERR_NO_MEM, TAG, "codec handle");
    es8311_clock_config_t clock = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * 256,
        .sample_frequency = SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(es8311_init(codec, &clock, ES8311_RESOLUTION_16,
                                    ES8311_RESOLUTION_16), TAG, "ES8311 init");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec, false),
                        TAG, "analog microphone");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_42DB),
                        TAG, "microphone gain");
    ESP_LOGI(TAG, "ES8311 capture ready: 24 kHz mono PCM16");
    return ESP_OK;
}

void app_main(void)
{
    serial_control_init();
    ESP_ERROR_CHECK(init_audio());
    uac_device_config_t usb = {
        .input_cb = microphone_input,
        .mic_itf_num = 1,
    };
    ESP_ERROR_CHECK(uac_device_init(&usb));
    ESP_LOGI(TAG, "USB audio microphone ready");
    esp_err_t display_result = display_show_microphone(capture_idle_peak);
    if (display_result != ESP_OK) {
        ESP_LOGW(TAG, "status display unavailable: %s",
                 esp_err_to_name(display_result));
    }
}
