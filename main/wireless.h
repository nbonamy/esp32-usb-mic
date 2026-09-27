#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WIRELESS_FRAMES 240
#define WIRELESS_PORT 24242

esp_err_t wireless_init(void);
bool wireless_is_streaming(void);
void wireless_clear_audio(void);
void wireless_submit(const int16_t *samples, size_t frames);
bool wireless_configured(void);
esp_err_t wireless_set_credentials(const char *ssid, const char *password);
esp_err_t wireless_clear_credentials(void);
