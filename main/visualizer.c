#include "visualizer.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#define WAVE_BARS 62
#define AUDIO_COUNT 240
#define SPECTRUM_COUNT 12
#define RING_POINTS 256
#define RING_BASE_RADIUS 94

static portMUX_TYPE audio_guard = portMUX_INITIALIZER_UNLOCKED;
static uint8_t latest_height;
static uint8_t pending_height;
static int16_t latest_audio[AUDIO_COUNT];

// Prepared once per display frame. Only the display task reads these fields.
static uint8_t frame_history[WAVE_BARS];
static int16_t frame_audio[AUDIO_COUNT];
static uint8_t bands[SPECTRUM_COUNT];
static uint8_t loudness;
static unsigned current_mode;
static uint32_t phase;
static int16_t sine_lut[256];
static int16_t ribbon_top[3][VISUALIZER_WIDTH];
static int16_t ribbon_bottom[3][VISUALIZER_WIDTH];
static int16_t ring_base_x[RING_POINTS];
static int16_t ring_base_y[RING_POINTS];
static int16_t ring_edge_x[RING_POINTS];
static int16_t ring_edge_y[RING_POINTS];
static uint16_t ring_colors[RING_POINTS];
static float spectrum_coefficients[SPECTRUM_COUNT];

static const uint8_t spectrum_bins[SPECTRUM_COUNT] = {
    1, 2, 3, 4, 5, 7, 9, 12, 16, 22, 30, 40,
};
static const uint16_t spectrum_colors[SPECTRUM_COUNT] = {
    0x07FF, 0x063F, 0x04BF, 0x43FF, 0xA35F, 0xE0FF,
    0xF81F, 0xF86F, 0xF9A8, 0xFC40, 0xFFE0, 0xF9C9,
};
static const uint16_t ribbon_colors[8] = {
    0x07FF, 0x063F, 0x347F, 0xA19F, 0xF81F, 0xF86F, 0xFC40, 0xFD52,
};
static int magnitude(int16_t sample)
{
    int value = sample;
    return value < 0 ? -value : value;
}

static uint8_t peak_height(int peak)
{
    int height;
    if (peak < 400) height = peak / 80;
    else if (peak < 4000) height = 5 + (peak - 400) / 55;
    else height = 70 + (peak - 4000) / 140;
    if (height > 108) height = 108;
    if (height < 2) height = 2;
    return height;
}

static int wave(int index)
{
    return sine_lut[(uint8_t)index];
}

static uint16_t dim_color(uint16_t color, unsigned shift)
{
    return (uint16_t)((((color >> 11) & 31) >> shift) << 11 |
                      (((color >> 5) & 63) >> shift) << 5 |
                      ((color & 31) >> shift));
}

static uint16_t scale_color(uint16_t color, unsigned level)
{
    return (uint16_t)((((color >> 11) & 31) * level / 64) << 11 |
                      (((color >> 5) & 63) * level / 64) << 5 |
                      ((color & 31) * level / 64));
}

static uint16_t blend_color(uint16_t first, uint16_t second, unsigned fraction)
{
    unsigned inverse = 256 - fraction;
    unsigned red = (((first >> 11) & 31) * inverse +
                    ((second >> 11) & 31) * fraction) / 256;
    unsigned green = (((first >> 5) & 63) * inverse +
                      ((second >> 5) & 63) * fraction) / 256;
    unsigned blue = ((first & 31) * inverse +
                     (second & 31) * fraction) / 256;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

void visualizer_init(void)
{
    for (int i = 0; i < 256; ++i) {
        sine_lut[i] = (int16_t)lroundf(sinf((2.0f * 3.14159265f * i) / 256.0f) *
                                       1024.0f);
    }
    for (int angle = 0; angle < RING_POINTS; ++angle) {
        ring_base_x[angle] = 184 + RING_BASE_RADIUS * wave(angle + 64) / 1024;
        ring_base_y[angle] = 224 + RING_BASE_RADIUS * wave(angle) / 1024;
    }
    for (int i = 0; i < SPECTRUM_COUNT; ++i) {
        spectrum_coefficients[i] = 2.0f * cosf(2.0f * 3.14159265f *
                                              spectrum_bins[i] / AUDIO_COUNT);
    }
}

void visualizer_record_audio(const int16_t *samples, size_t count)
{
    int peak = 0;
    for (size_t i = 0; samples && i < count; ++i) {
        int value = magnitude(samples[i]);
        if (value > peak) peak = value;
    }

    portENTER_CRITICAL(&audio_guard);
    latest_height = peak_height(peak);
    if (latest_height > pending_height) pending_height = latest_height;
    memset(latest_audio, 0, sizeof(latest_audio));
    if (samples && count) {
        size_t copied = count > AUDIO_COUNT ? AUDIO_COUNT : count;
        memcpy(latest_audio + AUDIO_COUNT - copied, samples + count - copied,
               copied * sizeof(int16_t));
    }
    portEXIT_CRITICAL(&audio_guard);
}

static void prepare_spectrum(void)
{
    static const float thresholds[16] = {
        18, 28, 42, 62, 90, 130, 185, 260,
        360, 500, 680, 920, 1230, 1640, 2180, 2900,
    };
    for (int band = 0; band < SPECTRUM_COUNT; ++band) {
        float coefficient = spectrum_coefficients[band];
        float previous = 0;
        float before_previous = 0;
        for (int i = 0; i < AUDIO_COUNT; ++i) {
            float next = frame_audio[i] + coefficient * previous - before_previous;
            before_previous = previous;
            previous = next;
        }
        float power = previous * previous + before_previous * before_previous -
                      coefficient * previous * before_previous;
        float amplitude = (2.0f / AUDIO_COUNT) * sqrtf(fmaxf(power, 0)) *
                          (0.8f + band * 0.10f);
        uint8_t target = 0;
        while (target < 16 && amplitude > thresholds[target]) ++target;
        // Fast attack and slower fall keep speech from flickering excessively.
        if (target >= bands[band]) bands[band] = target;
        else if (bands[band]) --bands[band];
    }
}

static void prepare_ribbons(void)
{
    int amplitude = 20 + loudness / 2;
    for (int x = 0; x < VISUALIZER_WIDTH; ++x) {
        for (int layer = 0; layer < 3; ++layer) {
            int first = wave(x * (2 + layer) / 3 + phase * (2 + layer) + layer * 72);
            int second = wave(x * (4 + layer) / 4 - phase * 2 + layer * 43);
            int center = 224 + (first * amplitude + second * amplitude / 3) / 1024;
            int thickness = 4 + loudness / 15 + layer * 2;
            ribbon_top[layer][x] = center - thickness;
            ribbon_bottom[layer][x] = center + thickness;
        }
    }
}

static void prepare_ring(void)
{
    // Each angular sector follows one measured frequency band. Interpolating
    // the neighboring bands keeps the circle continuous as its edge pulses.
    for (int angle = 0; angle < RING_POINTS; ++angle) {
        unsigned position = (unsigned)angle * SPECTRUM_COUNT;
        unsigned band = position / RING_POINTS;
        unsigned fraction = position % RING_POINTS;
        unsigned next = (band + 1) % SPECTRUM_COUNT;
        unsigned level = (bands[band] * (RING_POINTS - fraction) +
                          bands[next] * fraction) / RING_POINTS;
        unsigned radius = RING_BASE_RADIUS + level * 4;
        ring_edge_x[angle] = 184 + radius * wave(angle + 64) / 1024;
        ring_edge_y[angle] = 224 + radius * wave(angle) / 1024;
        uint16_t hue = blend_color(spectrum_colors[band],
                                   spectrum_colors[next], fraction);
        ring_colors[angle] = scale_color(hue, 36 + level * 28 / 16);
    }
}

void visualizer_prepare(unsigned mode, uint32_t frame_number)
{
    current_mode = mode % VISUALIZER_MODE_COUNT;
    phase = frame_number;
    portENTER_CRITICAL(&audio_guard);
    uint8_t new_bar = pending_height;
    pending_height = 0;
    uint8_t current_height = latest_height;
    memcpy(frame_audio, latest_audio, sizeof(frame_audio));
    portEXIT_CRITICAL(&audio_guard);
    // One bar advances per display frame, regardless of the 10 ms audio rate.
    // The tallest peak since the previous frame enters from the right.
    memmove(frame_history, frame_history + 1, WAVE_BARS - 1);
    frame_history[WAVE_BARS - 1] = new_bar > 2 ? new_bar : 2;
    loudness = current_height;
    if (current_mode == 1 || current_mode == 3) prepare_spectrum();
    if (current_mode == 2) prepare_ribbons();
    if (current_mode == 3) prepare_ring();
}

static void fill_column(uint16_t *pixels, int top, int rows,
                        int x, int first_y, int last_y, uint16_t color)
{
    if (x < 0 || x >= VISUALIZER_WIDTH) return;
    if (first_y < top) first_y = top;
    if (last_y >= top + rows) last_y = top + rows - 1;
    for (int y = first_y; y <= last_y; ++y) {
        pixels[(y - top) * VISUALIZER_WIDTH + x] = color;
    }
}

static void draw_waveform_stripe(uint16_t *pixels, int top, int rows)
{
    for (int x = 12; x < 356; ++x) {
        uint16_t hue = ribbon_colors[(x * 6) / VISUALIZER_WIDTH];
        fill_column(pixels, top, rows, x, 223, 225, dim_color(hue, 3));
    }
    for (int bar = 0; bar < WAVE_BARS; ++bar) {
        int height = frame_history[bar] > 2 ? frame_history[bar] : 2;
        for (int lane = 0; lane <= 4; ++lane) {
            int x = bar * 6 + lane;
            if (x >= VISUALIZER_WIDTH) break;
            uint16_t hue = ribbon_colors[(x * 6) / VISUALIZER_WIDTH];
            int outer_top = 224 - height - 7;
            int outer_bottom = 224 + height + 7;
            if (outer_top < 112) outer_top = 112;
            if (outer_bottom > 335) outer_bottom = 335;
            fill_column(pixels, top, rows, x, outer_top, outer_bottom,
                        dim_color(hue, 3));
            if (lane <= 3) {
                fill_column(pixels, top, rows, x, 224 - height - 3,
                            224 + height + 3, dim_color(hue, 2));
            }
            if (lane <= 2) {
                fill_column(pixels, top, rows, x, 224 - height,
                            224 + height, hue);
            }
        }
    }
}

static void draw_spectrum_stripe(uint16_t *pixels, int top, int rows)
{
    for (int band = 0; band < SPECTRUM_COUNT; ++band) {
        int left = 22 + band * 27;
        for (int cell = 0; cell < bands[band]; ++cell) {
            int first_y = 367 - cell * 18;
            int last_y = 379 - cell * 18;
            if (first_y < top) first_y = top;
            if (last_y >= top + rows) last_y = top + rows - 1;
            for (int x = left; x < left + 19; ++x) {
                fill_column(pixels, top, rows, x, first_y, last_y,
                            spectrum_colors[band]);
            }
        }
    }
}

static void draw_ribbons_stripe(uint16_t *pixels, int top, int rows)
{
    for (int layer = 0; layer < 3; ++layer) {
        for (int x = 0; x < VISUALIZER_WIDTH; ++x) {
            int first_y = ribbon_top[layer][x];
            int last_y = ribbon_bottom[layer][x];
            if (first_y < 88) first_y = 88;
            if (last_y > 359) last_y = 359;
            uint16_t hue = ribbon_colors[(x * 8 / VISUALIZER_WIDTH + layer) % 8];
            fill_column(pixels, top, rows, x, first_y, last_y,
                        dim_color(hue, 2));
            fill_column(pixels, top, rows, x, first_y, first_y + 2, hue);
            fill_column(pixels, top, rows, x, last_y - 2, last_y, hue);
        }
    }
}

static void ring_stripe_pixel(uint16_t *pixels, int top, int rows,
                              int x, int y, uint16_t color)
{
    if (x < 0 || x >= VISUALIZER_WIDTH || y < top || y >= top + rows) return;
    pixels[(y - top) * VISUALIZER_WIDTH + x] = color;
}

static void ring_stripe_line(uint16_t *pixels, int top, int rows,
                             int x0, int y0, int x1, int y1, uint16_t color)
{
    if ((y0 < top - 1 && y1 < top - 1) ||
        (y0 > top + rows && y1 > top + rows)) return;
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        ring_stripe_pixel(pixels, top, rows, x0, y0, color);
        ring_stripe_pixel(pixels, top, rows, x0 - 1, y0, color);
        ring_stripe_pixel(pixels, top, rows, x0 + 1, y0, color);
        ring_stripe_pixel(pixels, top, rows, x0, y0 - 1, color);
        ring_stripe_pixel(pixels, top, rows, x0, y0 + 1, color);
        if (x0 == x1 && y0 == y1) break;
        int doubled = 2 * error;
        if (doubled >= dy) { error += dy; x0 += sx; }
        if (doubled <= dx) { error += dx; y0 += sy; }
    }
}

void visualizer_draw_stripe(uint16_t *pixels, int top, int rows)
{
    memset(pixels, 0, (size_t)rows * VISUALIZER_WIDTH * sizeof(uint16_t));
    if (current_mode == 0) {
        draw_waveform_stripe(pixels, top, rows);
        return;
    }
    if (current_mode == 1) {
        draw_spectrum_stripe(pixels, top, rows);
        return;
    }
    if (current_mode == 2) {
        draw_ribbons_stripe(pixels, top, rows);
        return;
    }
    // The quiet circle remains visible; frequency energy lifts its colored
    // edge outward at the corresponding angle.
    for (int angle = 0; angle < RING_POINTS; ++angle) {
        int next = (angle + 1) % RING_POINTS;
        ring_stripe_line(pixels, top, rows,
                         ring_base_x[angle], ring_base_y[angle],
                         ring_base_x[next], ring_base_y[next],
                         dim_color(ring_colors[angle], 2));
    }
    for (int angle = 0; angle < RING_POINTS; ++angle) {
        int next = (angle + 1) % RING_POINTS;
        ring_stripe_line(pixels, top, rows,
                         ring_edge_x[angle], ring_edge_y[angle],
                         ring_edge_x[next], ring_edge_y[next],
                         ring_colors[angle]);
    }
}
