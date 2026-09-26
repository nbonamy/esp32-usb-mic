#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "tusb.h"

// The USB CDC function is a control path for recovery. It deliberately does
// not mirror the hardware Serial/JTAG port: the ESP32-S3 shares one USB PHY.
// A distinct command avoids rebooting when a terminal simply opens the port.
#define ROM_COMMAND "MIC BOOTLOADER"

static bool reboot_pending;
static char command[sizeof(ROM_COMMAND)];
static size_t command_length;

void serial_control_init(void)
{
    reboot_pending = false;
    command_length = 0;
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

void tud_cdc_rx_cb(uint8_t interface_number)
{
    if (interface_number != 0) return;

    while (tud_cdc_available()) {
        const int ch = tud_cdc_read_char();
        if (ch == '\r') continue;
        if (ch == '\n') {
            command[command_length] = '\0';
            if (!reboot_pending && strcmp(command, ROM_COMMAND) == 0) {
                if (xTaskCreate(reboot_to_rom, "rom_reboot", 3072, NULL, 5,
                                NULL) == pdPASS) {
                    reboot_pending = true;
                    tud_cdc_write_str("Entering ROM download mode\r\n");
                    tud_cdc_write_flush();
                } else {
                    tud_cdc_write_str("Reboot task unavailable\r\n");
                    tud_cdc_write_flush();
                }
            } else if (strcmp(command, "PING") == 0) {
                tud_cdc_write_str("Waveshare USB Microphone\r\n");
                tud_cdc_write_flush();
            }
            command_length = 0;
            continue;
        }
        if (command_length < sizeof(command) - 1) {
            command[command_length++] = (char)ch;
        } else {
            command_length = 0;
        }
    }
}
