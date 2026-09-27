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
#define RING_INNER_RADIUS 116
#define RING_BASE_RADIUS 130
#define WATERFALL_ROWS 32

static portMUX_TYPE audio_guard = portMUX_INITIALIZER_UNLOCKED;
static uint8_t latest_height;
static uint8_t pending_height;
static int16_t latest_audio[AUDIO_COUNT];

// Prepared once per display frame. Only the display task reads these fields.
static uint8_t frame_history[WAVE_BARS];
static uint16_t frame_hues[WAVE_BARS];
static int16_t frame_audio[AUDIO_COUNT];
static uint8_t bands[SPECTRUM_COUNT];
static uint8_t spectrum_glow[SPECTRUM_COUNT][16];
static uint8_t spectrum_levels[SPECTRUM_COUNT];
static uint8_t waterfall[WATERFALL_ROWS][SPECTRUM_COUNT];
static uint8_t waterfall_energy[WATERFALL_ROWS];
static uint8_t loudness;
static unsigned current_mode;
static uint32_t phase;
static int16_t sine_lut[256];
static int16_t ribbon_top[3][VISUALIZER_WIDTH];
static int16_t ribbon_bottom[3][VISUALIZER_WIDTH];
static uint8_t ring_outer_radius[RING_POINTS];
static uint16_t ring_colors[RING_POINTS][16];
static uint8_t angle_lut[256];
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
static const uint16_t ring_palette[8] = {
    0x07E0, 0x07FF, 0x001F, 0x781F,
    0xF81F, 0xF800, 0xFFE0, 0xAFE0,
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

static uint16_t palette_color(const uint16_t *palette, unsigned count,
                              unsigned position)
{
    unsigned index = (position >> 8) % count;
    unsigned next = (index + 1) % count;
    return blend_color(palette[index], palette[next], position & 255);
}

void visualizer_init(void)
{
    for (int i = 0; i < 256; ++i) {
        sine_lut[i] = (int16_t)lroundf(sinf((2.0f * 3.14159265f * i) / 256.0f) *
                                       1024.0f);
    }
    for (int i = 0; i < SPECTRUM_COUNT; ++i) {
        spectrum_coefficients[i] = 2.0f * cosf(2.0f * 3.14159265f *
                                              spectrum_bins[i] / AUDIO_COUNT);
    }
    for (int i = 0; i < 256; ++i) {
        angle_lut[i] = (uint8_t)lroundf(atanf(i / 255.0f) * 128.0f /
                                         3.14159265f);
    }
    for (int i = 0; i < WAVE_BARS; ++i) frame_hues[i] = ribbon_colors[0];
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
        spectrum_levels[band] = target;
        // Fast attack and slower fall keep speech from flickering excessively.
        if (target >= bands[band]) bands[band] = target;
        else if (bands[band]) --bands[band];
        for (int cell = 0; cell < 16; ++cell) {
            uint8_t *glow = &spectrum_glow[band][cell];
            if (cell < target) *glow = 64;
            else *glow = *glow > 8 ? *glow - 8 : 0;
        }
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
        unsigned outer_radius = RING_BASE_RADIUS + level * 2;
        if (outer_radius > 158) outer_radius = 158;
        ring_outer_radius[angle] = outer_radius;
        uint16_t hue = palette_color(ring_palette, 8, angle * 8);
        for (int shade = 0; shade < 16; ++shade) {
            ring_colors[angle][shade] = blend_color(0xFFFF, hue,
                                                    shade * 256 / 15);
        }
    }
}

static void prepare_waterfall(void)
{
    memmove(waterfall[1], waterfall[0],
            sizeof(waterfall) - sizeof(waterfall[0]));
    memmove(waterfall_energy + 1, waterfall_energy,
            sizeof(waterfall_energy) - sizeof(waterfall_energy[0]));
    memcpy(waterfall[0], spectrum_levels, sizeof(waterfall[0]));
    uint8_t energy = 0;
    for (int band = 0; band < SPECTRUM_COUNT; ++band) {
        if (spectrum_levels[band] > energy) energy = spectrum_levels[band];
    }
    waterfall_energy[0] = energy;
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
    memmove(frame_hues, frame_hues + 1,
            (WAVE_BARS - 1) * sizeof(frame_hues[0]));
    frame_history[WAVE_BARS - 1] = new_bar > 2 ? new_bar : 2;
    frame_hues[WAVE_BARS - 1] = palette_color(ribbon_colors, 8,
                                              frame_number * 24);
    loudness = current_height;
    if (current_mode == 1 || current_mode == 3 || current_mode == 4) prepare_spectrum();
    if (current_mode == 2) prepare_ribbons();
    if (current_mode == 3) prepare_ring();
    if (current_mode == 4) prepare_waterfall();
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
        uint16_t hue = frame_hues[x / 6];
        fill_column(pixels, top, rows, x, 223, 225, dim_color(hue, 3));
    }
    for (int bar = 0; bar < WAVE_BARS; ++bar) {
        int height = frame_history[bar] > 2 ? frame_history[bar] : 2;
        for (int lane = 0; lane <= 4; ++lane) {
            int x = bar * 6 + lane;
            if (x >= VISUALIZER_WIDTH) break;
            uint16_t hue = frame_hues[bar];
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
        for (int cell = 0; cell < 16; ++cell) {
            uint8_t glow = spectrum_glow[band][cell];
            if (!glow) continue;
            int first_y = 367 - cell * 18;
            int last_y = 379 - cell * 18;
            if (first_y < top) first_y = top;
            if (last_y >= top + rows) last_y = top + rows - 1;
            uint16_t color = scale_color(spectrum_colors[band], glow);
            for (int x = left; x < left + 19; ++x) {
                fill_column(pixels, top, rows, x, first_y, last_y,
                            color);
            }
        }
    }
}

static void draw_ribbons_stripe(uint16_t *pixels, int top, int rows)
{
    for (int x = 12; x < VISUALIZER_WIDTH - 12; ++x) {
        uint16_t hue = palette_color(ribbon_colors, 8,
                                     (unsigned)x * 8 * 256 / VISUALIZER_WIDTH);
        fill_column(pixels, top, rows, x, 224, 224, dim_color(hue, 3));
    }
    for (int layer = 0; layer < 3; ++layer) {
        for (int x = 0; x < VISUALIZER_WIDTH; ++x) {
            int first_y = ribbon_top[layer][x];
            int last_y = ribbon_bottom[layer][x];
            if (first_y < 88) first_y = 88;
            if (last_y > 359) last_y = 359;
            int center = (first_y + last_y) / 2;
            int half = (last_y - first_y) / 2;
            uint16_t hue = palette_color(ribbon_colors, 8,
                (unsigned)x * 8 * 256 / VISUALIZER_WIDTH + layer * 256);
            fill_column(pixels, top, rows, x, first_y, last_y,
                        dim_color(hue, 3));
            fill_column(pixels, top, rows, x, center - half * 2 / 3,
                        center + half * 2 / 3, dim_color(hue, 2));
            fill_column(pixels, top, rows, x, center - half / 3,
                        center + half / 3, dim_color(hue, 1));
            fill_column(pixels, top, rows, x, center, center, hue);
        }
    }
}

static unsigned ring_angle(int dx, int dy)
{
    unsigned ax = abs(dx);
    unsigned ay = abs(dy);
    unsigned quarter = ax >= ay ?
        angle_lut[ay * 255 / ax] : 64 - angle_lut[ax * 255 / ay];
    if (dx >= 0) return dy >= 0 ? quarter : (256 - quarter) & 255;
    return dy >= 0 ? 128 - quarter : 128 + quarter;
}

static void draw_waterfall_stripe(uint16_t *pixels, int top, int rows)
{
    for (int row = 0; row < WATERFALL_ROWS; ++row) {
        int first_y = 64 + row * 10;
        int last_y = first_y + 7;
        if (last_y < top || first_y >= top + rows) continue;
        unsigned energy = waterfall_energy[row];
        if (!energy) continue;
        for (int band = 0; band < SPECTRUM_COUNT; ++band) {
            unsigned level = waterfall[row][band];
            unsigned brightness = (8 + energy + level * 2) *
                                  (64 - row / 2) / 64;
            uint16_t color = scale_color(spectrum_colors[band], brightness);
            int left = 22 + band * 27;
            for (int x = left; x < left + 23; ++x) {
                fill_column(pixels, top, rows, x, first_y, last_y, color);
            }
        }
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
    if (current_mode == 4) {
        draw_waterfall_stripe(pixels, top, rows);
        return;
    }
    // Rasterize only the annulus, using a small integer angle lookup. This
    // leaves a solid white inner rim and has no gaps between radial spokes.
    for (int y = top; y < top + rows; ++y) {
        int dy = y - 224;
        if (abs(dy) > 158) continue;
        int extent = (int)sqrtf(158 * 158 - dy * dy);
        for (int x = 184 - extent; x <= 184 + extent; ++x) {
            int dx = x - 184;
            int radius_squared = dx * dx + dy * dy;
            if (radius_squared < RING_INNER_RADIUS * RING_INNER_RADIUS) continue;
            unsigned angle = ring_angle(dx, dy);
            int outer = ring_outer_radius[angle];
            if (radius_squared > outer * outer) continue;
            int start = RING_INNER_RADIUS + 8;
            int shade = radius_squared <= start * start ? 0 :
                (radius_squared - start * start) * 15 /
                (outer * outer - start * start);
            pixels[(y - top) * VISUALIZER_WIDTH + x] =
                ring_colors[angle][shade];
        }
    }
}
