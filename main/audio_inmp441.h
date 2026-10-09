#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define AUDIO_PITCH_FRAME_SAMPLES 4096

typedef enum {
    AUDIO_ROUTE_IDLE = 0,
    AUDIO_ROUTE_PITCH,
    AUDIO_ROUTE_VOICE,
} audio_route_t;

typedef struct {
    float vad_threshold;
    float raw_rms;
    float gain_applied;
    uint32_t duration_ms;
    bool speech_detected;
} audio_voice_capture_info_t;

esp_err_t audio_inmp441_init(void);
void audio_inmp441_set_route(audio_route_t route);
esp_err_t audio_inmp441_get_pitch_frame(float *destination, TickType_t timeout);

/*
 * 语音命令采集：
 * - 等待说话开始；
 * - 检测到尾部静音后自动停止；
 * - 自动裁剪首尾静音；
 * - 自动增益到适合云端ASR的幅度。
 *
 * 没检测到有效语音时返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t audio_inmp441_capture_voice(
    int16_t *destination,
    size_t sample_capacity,
    size_t *samples_written,
    audio_voice_capture_info_t *capture_info,
    TickType_t timeout
);

float audio_inmp441_last_rms(void);
float audio_inmp441_noise_rms(void);
int32_t audio_inmp441_last_peak(void);

/* Short, timestamped envelope windows for rhythm; pitch stays at 4096 samples. */
typedef struct { int64_t center_us; float rms, noise; bool onset; } audio_rhythm_frame_t;
esp_err_t audio_inmp441_get_rhythm_frame(audio_rhythm_frame_t *frame, TickType_t timeout);
