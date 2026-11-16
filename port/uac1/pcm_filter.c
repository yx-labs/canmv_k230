#include "pcm_filter.h"

/* 127-tap Kaiser-windowed sinc, cutoff 6850 Hz at 48 kHz, beta 7.85.
 * Quantized to Q15 with sum 32768. Minimum rejection from 8 kHz is 66 dB.
 * The same response rejects capture aliases and playback images. */
static const int16_t fir[127] = {
    0,-1,-1,-1,1,2,2,0,-4,-6,-3,4,10,9,-1,-14,-19,-9,12,30,26,-2,-36,-48,
    -23,29,69,60,-3,-78,-104,-48,60,141,121,-5,-152,-203,-94,114,268,230,-6,
    -283,-379,-179,210,502,435,-8,-541,-739,-360,420,1050,955,-9,-1320,-1985,
    -1106,1505,5068,8141,9360,8141,5068,1505,-1106,-1985,-1320,-9,955,1050,
    420,-360,-739,-541,-8,435,502,210,-179,-379,-283,-6,230,268,114,-94,-203,
    -152,-5,121,141,60,-48,-104,-78,-3,60,69,29,-23,-48,-36,-2,26,30,12,-9,
    -19,-14,-1,9,10,4,-3,-6,-4,0,2,2,1,-1,-1,-1,0
};

static int16_t sample_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_le(uint8_t *p, int32_t v)
{
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static void push(struct pcm_fir *s, int16_t x)
{
    s->head = s->head ? s->head - 1 : 126;
    s->delay[s->head] = x;
}

static int32_t filter(const struct pcm_fir *s, unsigned start, unsigned stride, unsigned gain)
{
    unsigned j = s->head;
    int64_t sum = 0;
    for (unsigned i = start; i < 127; i += stride) {
        sum += (int32_t)fir[i] * s->delay[j];
        if (++j == 127) j = 0;
    }
    sum *= gain;
    return (int32_t)((sum + (sum < 0 ? -16384 : 16384)) / 32768);
}

size_t pcm_down3(struct pcm_fir *s, const uint8_t *src, size_t frames,
                 unsigned channels, uint8_t *dst)
{
    size_t count = 0;
    for (size_t i = 0; i < frames; ++i, src += 2 * channels) {
        int32_t x = sample_le(src);
        if (channels == 2) x = (x + sample_le(src + 2)) / 2;
        push(s, (int16_t)x);
        if (++s->phase == 3) {
            s->phase = 0;
            write_le(dst + count * 2, filter(s, 0, 1, 1));
            ++count;
        }
    }
    return count;
}

void pcm_up3(struct pcm_fir *s, const uint8_t *src, size_t samples,
             unsigned channels, unsigned volume, uint8_t *dst)
{
    for (size_t i = 0; i < samples; ++i) {
        push(s, sample_le(src + i * 2));
        for (unsigned p = 0; p < 3; ++p) {
            const int32_t y = filter(s, p, 3, 3) * (int32_t)volume / 100;
            for (unsigned c = 0; c < channels; ++c, dst += 2) write_le(dst, y);
        }
    }
}
