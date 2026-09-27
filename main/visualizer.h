#pragma once

#include <stddef.h>
#include <stdint.h>

#define VISUALIZER_WIDTH 368
#define VISUALIZER_HEIGHT 448
#define VISUALIZER_MODE_COUNT 4

void visualizer_init(void);
void visualizer_record_audio(const int16_t *samples, size_t count);
void visualizer_prepare(unsigned mode, uint32_t frame_number);
// Fill a full-width stripe in host-order RGB565. The panel transport swaps bytes.
void visualizer_draw_stripe(uint16_t *pixels, int top, int rows);
