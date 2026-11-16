/* Streaming PCM16 conversion for the 48 kHz USB and 16 kHz application clocks. */
#ifndef CANMV_UAC1_PCM_FILTER_H
#define CANMV_UAC1_PCM_FILTER_H
#include <stdint.h>
#include <stddef.h>

struct pcm_fir {
    int16_t delay[127];
    unsigned head;
    unsigned phase;
};

size_t pcm_down3(struct pcm_fir *state, const uint8_t *src, size_t frames,
                 unsigned channels, uint8_t *dst);
void pcm_up3(struct pcm_fir *state, const uint8_t *src, size_t samples,
             unsigned channels, unsigned volume, uint8_t *dst);
#endif
