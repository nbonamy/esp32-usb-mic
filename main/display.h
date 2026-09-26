#pragma once

#include "esp_err.h"

// Draw the static microphone identity after USB starts. Audio remains usable
// if the panel is unavailable.
esp_err_t display_show_microphone(void);
