#include "wireless.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "power_button.h"

#define DISCOVERY "WMIC_DISCOVER_V1"
#define READY "WMIC_READY_V1"
#define ACTIVE "WMIC_ACTIVE_V1"
#define IDLE "WMIC_IDLE_V1"
#define POWER_QUERY "WMIC_POWER_V1"
#define PACKET_BYTES (8 + WIRELESS_FRAMES * sizeof(int16_t))

static const char *TAG = "wireless_mic";
static QueueHandle_t audio_queue;
static volatile bool wifi_ready;
static volatile TickType_t receiver_tick;
static volatile TickType_t active_tick;
static uint32_t sequence;

typedef struct {
    uint32_t sequence;
    int16_t samples[WIRELESS_FRAMES];
} audio_block_t;

static esp_err_t open_settings(nvs_handle_t *handle)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init");
    return nvs_open("mic_wifi", NVS_READWRITE, handle);
}

bool wireless_configured(void)
{
    nvs_handle_t nvs;
    if (open_settings(&nvs) != ESP_OK) return false;
    size_t length = 0;
    bool configured = nvs_get_str(nvs, "ssid", NULL, &length) == ESP_OK &&
                      length > 1;
    nvs_close(nvs);
    return configured;
}

esp_err_t wireless_set_credentials(const char *ssid, const char *password)
{
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (!ssid_length || ssid_length > 32 || password_length > 63 ||
        (password_length && password_length < 8)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings(&nvs), TAG, "NVS open");
    esp_err_t err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t wireless_clear_credentials(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings(&nvs), TAG, "NVS open");
    esp_err_t err = nvs_erase_key(nvs, "ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(nvs, "password");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool wireless_is_streaming(void)
{
    TickType_t receiver = receiver_tick;
    TickType_t active = active_tick;
    TickType_t now = xTaskGetTickCount();
    return wifi_ready && receiver != 0 && active != 0 &&
           (TickType_t)(now - receiver) < pdMS_TO_TICKS(3000) &&
           (TickType_t)(now - active) < pdMS_TO_TICKS(1500);
}

void wireless_clear_audio(void)
{
    if (audio_queue) xQueueReset(audio_queue);
}

void wireless_submit(const int16_t *samples, size_t frames)
{
    if (!audio_queue || !wireless_is_streaming() || frames != WIRELESS_FRAMES)
        return;
    audio_block_t block = {.sequence = sequence++};
    memcpy(block.samples, samples, sizeof(block.samples));
    // Never let network back-pressure block USB or I2S capture.
    xQueueSend(audio_queue, &block, 0);
}

static void wifi_event(void *context, esp_event_base_t base, int32_t id,
                       void *data)
{
    (void)context;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_ready = false;
        receiver_tick = 0;
        active_tick = 0;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_ready = true;
        ESP_LOGI(TAG, "Wi-Fi connected; waiting for receiver");
    }
}

static void network_task(void *context)
{
    (void)context;
    struct sockaddr_in receiver = {0};
    for (;;) {
        if (!wifi_ready) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (socket_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        struct sockaddr_in local = {
            .sin_family = AF_INET,
            .sin_port = htons(WIRELESS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
            close(socket_fd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool fast_receive = false;
        while (wifi_ready) {
            bool streaming = wireless_is_streaming();
            if (streaming != fast_receive) {
                timeout.tv_usec = streaming ? 10000 : 100000;
                setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                           &timeout, sizeof(timeout));
                fast_receive = streaming;
            }
            char message[32];
            struct sockaddr_in from;
            socklen_t from_length = sizeof(from);
            int count = recvfrom(socket_fd, message, sizeof(message), 0,
                                 (struct sockaddr *)&from, &from_length);
            if (count == sizeof(DISCOVERY) - 1 &&
                memcmp(message, DISCOVERY, count) == 0) {
                receiver = from;
                receiver_tick = xTaskGetTickCount();
                sendto(socket_fd, READY, sizeof(READY) - 1, 0,
                       (struct sockaddr *)&receiver, sizeof(receiver));
            } else if (count == sizeof(ACTIVE) - 1 &&
                       memcmp(message, ACTIVE, count) == 0) {
                receiver = from;
                receiver_tick = xTaskGetTickCount();
                active_tick = receiver_tick;
            } else if (count == sizeof(IDLE) - 1 &&
                       memcmp(message, IDLE, count) == 0 &&
                       from.sin_addr.s_addr == receiver.sin_addr.s_addr) {
                active_tick = 0;
                xQueueReset(audio_queue);
            } else if (count == sizeof(POWER_QUERY) - 1 &&
                       memcmp(message, POWER_QUERY, count) == 0) {
                power_button_status_t status;
                if (power_button_read_status(&status) == ESP_OK) {
                    char reply[64];
                    int length = snprintf(reply, sizeof(reply),
                                          POWER_QUERY " %u %u %u",
                                          status.battery_mv,
                                          status.battery_percent,
                                          status.vbus_good);
                    if (length > 0 && (size_t)length < sizeof(reply)) {
                        sendto(socket_fd, reply, length, 0,
                               (struct sockaddr *)&from, sizeof(from));
                    }
                }
            }
            audio_block_t block;
            while (xQueueReceive(audio_queue, &block, 0) == pdTRUE) {
                if (!wireless_is_streaming()) continue;
                uint8_t packet[PACKET_BYTES];
                memcpy(packet, "WMIC", 4);
                uint32_t network_sequence = htonl(block.sequence);
                memcpy(packet + 4, &network_sequence, sizeof(network_sequence));
                memcpy(packet + 8, block.samples, sizeof(block.samples));
                sendto(socket_fd, packet, sizeof(packet), 0,
                       (struct sockaddr *)&receiver, sizeof(receiver));
            }
        }
        close(socket_fd);
    }
}

esp_err_t wireless_init(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings(&nvs), TAG, "NVS open");
    char ssid[33] = {0};
    char password[64] = {0};
    size_t ssid_size = sizeof(ssid);
    size_t password_size = sizeof(password);
    esp_err_t err = nvs_get_str(nvs, "ssid", ssid, &ssid_size);
    if (err == ESP_OK) err = nvs_get_str(nvs, "password", password,
                                        &password_size);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Wi-Fi unconfigured; USB microphone remains available");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read Wi-Fi settings");

    audio_queue = xQueueCreate(12, sizeof(audio_block_t));
    ESP_RETURN_ON_FALSE(audio_queue, ESP_ERR_NO_MEM, TAG, "audio queue");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta(), ESP_ERR_NO_MEM,
                        TAG, "station netif");
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                        WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL),
                        TAG, "disconnect handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                        IP_EVENT_STA_GOT_IP, wifi_event, NULL),
                        TAG, "IP handler");
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "volatile station settings");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "station config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Wi-Fi connect");
    ESP_RETURN_ON_FALSE(xTaskCreate(network_task, "wifi_audio", 4096, NULL, 5,
                                    NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "network task");
    memset(password, 0, sizeof(password));
    ESP_LOGI(TAG, "Wi-Fi station started");
    return ESP_OK;
}
