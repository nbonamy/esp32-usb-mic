#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_device_uac.h"

#include "codec/es8311.h"
#include "display.h"
#include "power_button.h"
#include "serial_control.h"
#include "wireless.h"

#define SAMPLE_RATE 24000
#define FRAMES_PER_READ (SAMPLE_RATE / 100)  // UAC asks for 10 ms.
#define I2C_PORT I2C_NUM_0
#define CODEC_ADDRESS ES8311_ADDRESS_0

static const char *TAG = "usb_mic";
static i2s_chan_handle_t rx_channel;
static es8311_handle_t codec;
static SemaphoreHandle_t audio_lock;
static bool microphone_paused;
static int16_t stereo[FRAMES_PER_READ * 2];
static uint32_t read_errors;
static volatile TickType_t last_usb_capture_tick;

static void stereo_to_mono(const int16_t *input, int16_t *output,
                           size_t frames)
{
    uint32_t energy[2] = {0, 0};
    for (size_t i = 0; i < frames; ++i) {
        energy[0] += abs((int)input[2 * i]);
        energy[1] += abs((int)input[2 * i + 1]);
    }
    const unsigned channel = energy[1] > energy[0] ? 1 : 0;
    for (size_t i = 0; i < frames; ++i) {
        output[i] = input[2 * i + channel];
    }
}

static esp_err_t microphone_input(uint8_t *buffer, size_t length,
                                  size_t *bytes_read, void *context)
{
    (void)context;
    *bytes_read = 0;
    if (length > FRAMES_PER_READ * sizeof(int16_t) || length % sizeof(int16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    last_usb_capture_tick = xTaskGetTickCount();

    xSemaphoreTake(audio_lock, portMAX_DELAY);
    if (microphone_paused) {
        memset(buffer, 0, length);
        *bytes_read = length;
        xSemaphoreGive(audio_lock);
        return ESP_OK;
    }

    const size_t frames = length / sizeof(int16_t);
    size_t received = 0;
    esp_err_t err = i2s_channel_read(rx_channel, stereo, frames * 2 * sizeof(int16_t),
                                     &received, pdMS_TO_TICKS(20));
    if (err != ESP_OK || received != frames * 2 * sizeof(int16_t)) {
        memset(buffer, 0, length);
        *bytes_read = length;
        display_record_audio(NULL, 0);
        if (++read_errors % 100 == 1) {
            ESP_LOGW(TAG, "I2S short read: %s, %u/%u bytes",
                     esp_err_to_name(err), (unsigned)received,
                     (unsigned)(frames * 2 * sizeof(int16_t)));
        }
        xSemaphoreGive(audio_lock);
        return ESP_OK;
    }

    // ES8311 capture presents stereo slots. The physical mic appears in the
    // stronger slot; follow the known-good Codex Remote channel selection.
    int16_t *mono = (int16_t *)buffer;
    stereo_to_mono(stereo, mono, frames);
    display_record_audio(mono, frames);
    wireless_submit(mono, frames);
    *bytes_read = length;
    xSemaphoreGive(audio_lock);
    return ESP_OK;
}

static void capture_idle_peak(void)
{
    // Keep the display reactive even when macOS has not opened the UAC stream.
    // USB capture has priority and owns I2S while it is active.
    if ((TickType_t)(xTaskGetTickCount() - last_usb_capture_tick) <
        pdMS_TO_TICKS(100)) return;
    if (wireless_is_streaming()) return;

    xSemaphoreTake(audio_lock, portMAX_DELAY);
    if (microphone_paused ||
        (TickType_t)(xTaskGetTickCount() - last_usb_capture_tick) <
            pdMS_TO_TICKS(100)) {
        xSemaphoreGive(audio_lock);
        return;
    }

    int16_t samples[FRAMES_PER_READ * 2];
    size_t received = 0;
    esp_err_t err = i2s_channel_read(rx_channel, samples, sizeof(samples),
                                     &received, pdMS_TO_TICKS(20));
    if (err != ESP_OK || received != sizeof(samples)) {
        xSemaphoreGive(audio_lock);
        return;
    }

    int16_t mono[FRAMES_PER_READ];
    stereo_to_mono(samples, mono, FRAMES_PER_READ);
    display_record_audio(mono, FRAMES_PER_READ);
    xSemaphoreGive(audio_lock);
}

static void wireless_capture_task(void *context)
{
    (void)context;
    TickType_t next = xTaskGetTickCount();
    int16_t input[FRAMES_PER_READ * 2];
    int16_t mono[FRAMES_PER_READ];
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(10));
        if (!wireless_is_streaming() ||
            (TickType_t)(xTaskGetTickCount() - last_usb_capture_tick) <
                pdMS_TO_TICKS(100)) continue;
        xSemaphoreTake(audio_lock, portMAX_DELAY);
        if (microphone_paused ||
            (TickType_t)(xTaskGetTickCount() - last_usb_capture_tick) <
                pdMS_TO_TICKS(100)) {
            xSemaphoreGive(audio_lock);
            continue;
        }
        size_t received = 0;
        esp_err_t err = i2s_channel_read(rx_channel, input, sizeof(input),
                                         &received, pdMS_TO_TICKS(20));
        if (err == ESP_OK && received == sizeof(input)) {
            stereo_to_mono(input, mono, FRAMES_PER_READ);
            display_record_audio(mono, FRAMES_PER_READ);
            wireless_submit(mono, FRAMES_PER_READ);
        }
        xSemaphoreGive(audio_lock);
    }
}

// audio_lock protects both pause requests and the codec/I2S transition.
static esp_err_t update_capture_locked(bool paused)
{
    if (microphone_paused == paused) return ESP_OK;
    if (paused) {
        // Refuse all further capture before touching the codec or I2S clocks.
        microphone_paused = true;
        esp_err_t codec_result = es8311_microphone_power_set(codec, false);
        esp_err_t i2s_result = i2s_channel_disable(rx_channel);
        if (codec_result != ESP_OK || i2s_result != ESP_OK) {
            ESP_LOGW(TAG, "pause power-down: codec=%s I2S=%s",
                     esp_err_to_name(codec_result), esp_err_to_name(i2s_result));
        }
    } else {
        esp_err_t err = i2s_channel_enable(rx_channel);
        if (err == ESP_OK) err = es8311_microphone_power_set(codec, true);
        if (err != ESP_OK) {
            i2s_channel_disable(rx_channel);
            return err;
        }
        microphone_paused = false;
        last_usb_capture_tick = 0;
    }

    ESP_LOGI(TAG, "microphone %s", paused ? "paused" : "resumed");
    return ESP_OK;
}

static esp_err_t set_microphone_paused(bool paused)
{
    xSemaphoreTake(audio_lock, portMAX_DELAY);
    esp_err_t err = update_capture_locked(paused);
    display_set_capture_active(!microphone_paused);
    xSemaphoreGive(audio_lock);
    if (paused) wireless_clear_audio();
    return err;
}

static void recording_control_task(void *context)
{
    (void)context;
    bool was_recording = false;
    for (;;) {
        bool recording = wireless_is_streaming();
        if (recording != was_recording) {
            esp_err_t err = set_microphone_paused(!recording);
            if (err == ESP_OK) was_recording = recording;
            else ESP_LOGW(TAG, "recording transition: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t init_audio(void)
{
    audio_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(audio_lock, ESP_ERR_NO_MEM, TAG, "audio mutex");
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

    codec = es8311_create(I2C_PORT, CODEC_ADDRESS);
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
    esp_err_t wireless_result = wireless_init();
    if (wireless_result != ESP_OK) {
        ESP_LOGW(TAG, "wireless microphone unavailable: %s",
                 esp_err_to_name(wireless_result));
    } else if (wireless_configured() &&
               xTaskCreate(wireless_capture_task, "wifi_capture", 4096, NULL,
                           6, NULL) != pdPASS) {
        ESP_LOGW(TAG, "wireless capture task unavailable");
    }
    esp_err_t power_result = power_button_init();
    if (power_result != ESP_OK) {
        ESP_LOGW(TAG, "PWR button unavailable: %s", esp_err_to_name(power_result));
    }
    // With Wi-Fi configured, start dark and silent on either power source.
    // The PWR button and a Mac input-open event can each turn capture on.
    if (wireless_configured()) ESP_ERROR_CHECK(set_microphone_paused(true));
    uac_device_config_t usb = {
        .input_cb = microphone_input,
        .mic_itf_num = 1,
    };
    ESP_ERROR_CHECK(uac_device_init(&usb));
    ESP_LOGI(TAG, "USB audio microphone ready");
    esp_err_t display_result = display_show_microphone(capture_idle_peak,
                                                       set_microphone_paused);
    if (display_result != ESP_OK) {
        ESP_LOGW(TAG, "status display unavailable: %s",
                 esp_err_to_name(display_result));
    }
    if (xTaskCreate(recording_control_task, "mic_control", 3072, NULL, 3,
                    NULL) != pdPASS) {
        ESP_LOGW(TAG, "recording control unavailable");
    }
}
