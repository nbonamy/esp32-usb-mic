#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// AXP2101-backed physical PWR button; uses the I2C bus initialized by audio.
esp_err_t power_button_init(void);
bool power_button_take_short_press(void);

typedef struct {
    bool battery_present;
    bool vbus_good;
    bool charging;
    uint16_t battery_mv;
    uint8_t battery_percent;
} power_button_status_t;

// Read the AXP2101's battery and external-power status for diagnostics.
esp_err_t power_button_read_status(power_button_status_t *status);
