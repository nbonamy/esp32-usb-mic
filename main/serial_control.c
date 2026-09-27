#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "tusb.h"
#include "power_button.h"
#include "wireless.h"

// The USB CDC function is a control path for recovery. It deliberately does
// not mirror the hardware Serial/JTAG port: the ESP32-S3 shares one USB PHY.
// A distinct command avoids rebooting when a terminal simply opens the port.
#define ROM_COMMAND "MIC BOOTLOADER"

static bool reboot_pending;
static char command[256];
static size_t command_length;
static bool command_overflow;

void serial_control_init(void)
{
    reboot_pending = false;
    command_length = 0;
    command_overflow = false;
}

static void rom_shutdown(void)
{
    // ESP-IDF's USB console uses this RTC bit to request ROM download mode.
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}

static void reboot_to_rom(void *context)
{
    (void)context;
    // Give the host time to receive the response and release the CDC port.
    vTaskDelay(pdMS_TO_TICKS(250));
    if (esp_register_shutdown_handler(rom_shutdown) != ESP_OK) {
        reboot_pending = false;
        vTaskDelete(NULL);
        return;
    }
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Return the shared internal PHY to the fixed USB Serial/JTAG controller.
    // The ROM download mode uses that controller, not our TinyUSB CDC function.
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL |
                            RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    esp_restart();
}

static void reboot_normal(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_restart();
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool decode_hex(const char *source, char *destination, size_t capacity)
{
    size_t length = strlen(source);
    if (length % 2 || length / 2 >= capacity) return false;
    for (size_t i = 0; i < length; i += 2) {
        int high = hex_digit(source[i]);
        int low = hex_digit(source[i + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
        destination[i / 2] = (char)((high << 4) | low);
    }
    destination[length / 2] = '\0';
    return true;
}

static void reply(const char *message)
{
    tud_cdc_write_str(message);
    tud_cdc_write_str("\r\n");
    tud_cdc_write_flush();
}

static void handle_command(void)
{
    if (!reboot_pending && strcmp(command, ROM_COMMAND) == 0) {
        if (xTaskCreate(reboot_to_rom, "rom_reboot", 3072, NULL, 5,
                        NULL) == pdPASS) {
            reboot_pending = true;
            reply("Entering ROM download mode");
        } else {
            reply("Reboot task unavailable");
        }
    } else if (strcmp(command, "PING") == 0) {
        reply("Waveshare USB Microphone");
    } else if (strcmp(command, "MIC WIFI STATUS") == 0) {
        reply(wireless_configured() ? "Wi-Fi configured" : "Wi-Fi unconfigured");
    } else if (strcmp(command, "MIC POWER") == 0) {
        power_button_status_t status;
        if (power_button_read_status(&status) == ESP_OK) {
            char text[128];
            snprintf(text, sizeof(text), "battery=%s voltage=%umV charge=%u%% vbus=%s charging=%s reset=%d",
                     status.battery_present ? "present" : "absent",
                     status.battery_mv, status.battery_percent,
                     status.vbus_good ? "yes" : "no",
                     status.charging ? "yes" : "no", esp_reset_reason());
            reply(text);
        } else {
            reply("Power status unavailable");
        }
    } else if (strcmp(command, "MIC WIFI CLEAR") == 0) {
        esp_err_t err = wireless_clear_credentials();
        if (err == ESP_OK && xTaskCreate(reboot_normal, "wifi_reboot", 2048,
                                         NULL, 5, NULL) == pdPASS) {
            reply("Wi-Fi cleared; restarting");
        } else {
            reply("Wi-Fi clear failed");
        }
    } else if (strncmp(command, "MIC WIFI SET ", 13) == 0) {
        char *ssid_hex = command + 13;
        char *separator = strchr(ssid_hex, ' ');
        char ssid[33];
        char password[64];
        if (!separator) {
            reply("Invalid Wi-Fi command");
            return;
        }
        *separator++ = '\0';
        if (!decode_hex(ssid_hex, ssid, sizeof(ssid)) ||
            !decode_hex(separator, password, sizeof(password))) {
            reply("Invalid Wi-Fi encoding");
            return;
        }
        esp_err_t err = wireless_set_credentials(ssid, password);
        memset(password, 0, sizeof(password));
        if (err == ESP_OK && xTaskCreate(reboot_normal, "wifi_reboot", 2048,
                                         NULL, 5, NULL) == pdPASS) {
            reply("Wi-Fi saved; restarting");
        } else {
            reply("Wi-Fi save failed");
        }
    } else {
        reply("Unknown command");
    }
}

void tud_cdc_rx_cb(uint8_t interface_number)
{
    if (interface_number != 0) return;

    while (tud_cdc_available()) {
        const int ch = tud_cdc_read_char();
        if (ch == '\r') continue;
        if (ch == '\n') {
            command[command_length] = '\0';
            if (command_overflow) reply("Command too long");
            else if (!reboot_pending) handle_command();
            command_length = 0;
            command_overflow = false;
            continue;
        }
        if (!command_overflow && command_length < sizeof(command) - 1) {
            command[command_length++] = (char)ch;
        } else {
            command_overflow = true;
        }
    }
}
