#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t xfyun_asr_validate_configuration(void);
esp_err_t xfyun_asr_transcribe(
    const int16_t *pcm,
    size_t sample_count,
    char *result_text,
    size_t result_capacity
);
