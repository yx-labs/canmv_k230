/* Build on the host with: cc -std=c99 -Wall -Wextra -Werror -Iport/uac1
 *   unit_test/uac1/pcm_filter_test.c port/uac1/pcm_filter.c -lm -o pcm_test */
#include "pcm_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#define SAMPLES 4800
static uint8_t stereo[SAMPLES * 4], mono[SAMPLES * 2], decoded[SAMPLES * 2];
static uint8_t chunks[SAMPLES * 2], expanded[SAMPLES * 12];
static const double pi = 3.141592653589793;
static void set16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)((uint16_t)v >> 8); }
static int get16(const uint8_t *p) { return (int16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static int tone_peak(double frequency)
{
    struct pcm_fir a = {0}, b = {0};
    for (unsigned i = 0; i < SAMPLES; ++i) {
        int value = (int)(24000.0 * sin(2.0 * pi * i * frequency / 48000.0));
        set16(stereo + 4 * i, value);
        set16(stereo + 4 * i + 2, value / 2);
    }
    size_t n = pcm_down3(&a, stereo, SAMPLES, 2, decoded);
    size_t cursor = 0, produced = 0;
    while (cursor < SAMPLES) {
        size_t batch = cursor % 19 + 1;
        if (cursor + batch > SAMPLES) batch = SAMPLES - cursor;
        produced += pcm_down3(&b, stereo + cursor * 4, batch, 2, chunks + produced * 2);
        cursor += batch;
    }
    assert(n == SAMPLES / 3 && n == produced && !memcmp(decoded, chunks, n * 2));
    int peak = 0;
    for (size_t i = 64; i < n; ++i) {
        int x = abs(get16(decoded + 2 * i));
        if (x > peak) peak = x;
    }
    return peak;
}

int main(void)
{
    int slow = tone_peak(1000), fast = tone_peak(11000);
    assert(slow > 17800 && slow < 18200 && fast < 18);
    for (unsigned i = 0; i < SAMPLES; ++i) set16(mono + 2 * i, 24000);
    struct pcm_fir a = {0}, b = {0};
    pcm_up3(&a, mono, SAMPLES, 2, 50, expanded);
    for (unsigned i = 200; i < SAMPLES * 3; ++i) {
        int left = get16(expanded + i * 4), right = get16(expanded + i * 4 + 2);
        assert(abs(left - 12000) < 12 && left == right);
    }
    uint8_t small[64 * 12];
    for (unsigned i = 0; i < SAMPLES; i += 64) {
        size_t count = (i + 64 > SAMPLES) ? SAMPLES - i : 64;
        pcm_up3(&b, mono + 2 * i, count, 2, 50, small);
        assert(!memcmp(small, expanded + i * 12, count * 12));
    }
    /* With all possible USB transfer boundaries the phase filter keeps the
     * application length, DC gain, stereo order, and 11 kHz alias rejection. */
    printf("1kHz downsample peak %d; 11kHz peak %d; playback stereo gain and chunk continuity OK\n", slow, fast);
    return 0;
}
