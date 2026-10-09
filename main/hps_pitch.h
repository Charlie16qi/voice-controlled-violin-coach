#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "note_utils.h"

typedef struct {
    bool valid;
    float frequency_hz;
    float cents;
    float confidence;
    float rms;
    float noise_gate;
    float best_score;
} hps_result_t;

esp_err_t hps_pitch_init(void);

esp_err_t hps_pitch_analyze(
    const float *samples,
    float target_hz,
    float noise_rms,
    hps_result_t *result
);

float pitch_cents(
    float frequency_hz,
    float target_hz
);
