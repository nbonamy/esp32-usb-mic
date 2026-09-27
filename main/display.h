#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// The optional callback samples the microphone when no USB host is reading it.
typedef void (*display_idle_capture_cb_t)(void);
typedef esp_err_t (*display_pause_cb_t)(bool paused);

// Initialize the panel and start its low-priority waveform renderer.
// Audio remains usable if the panel is unavailable.
esp_err_t display_show_microphone(display_idle_capture_cb_t idle_capture,
                                  display_pause_cb_t set_paused);

// Feed the loudest mono sample from each captured block to the display.
void display_record_peak(uint16_t peak);
