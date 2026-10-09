#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Envelope detector for a 10 ms PCM window. No FFT/target pitch dependency. */
typedef struct {
    float peak, previous;
    unsigned quiet;
    bool armed;
    int64_t last_onset_us;
} rhythm_onset_t;
void onset_reset(rhythm_onset_t *d);
bool onset_envelope(rhythm_onset_t *d, float rms, float noise,
                    int64_t center_us);
float onset_pcm_rms(const int16_t *pcm, size_t count);
