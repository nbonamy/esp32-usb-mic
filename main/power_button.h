#pragma once

#include <stdbool.h>

#include "esp_err.h"

// AXP2101-backed physical PWR button; uses the I2C bus initialized by audio.
esp_err_t power_button_init(void);
bool power_button_take_short_press(void);
