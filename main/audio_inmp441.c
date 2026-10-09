#include "audio_inmp441.h"
#include "wake_word.h"
#include "rhythm_onset.h"
#include "esp_timer.h"

#include <math.h>
#include <string.h>
#include "app_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "INMP441";

static i2s_chan_handle_t s_rx_channel;
static QueueHandle_t s_ready_frames;
static QueueHandle_t s_rhythm_frames;
static rhythm_onset_t s_onset;
static QueueHandle_t s_free_frames;
static SemaphoreHandle_t s_route_mutex;
static SemaphoreHandle_t s_voice_done;
static volatile audio_route_t s_route = AUDIO_ROUTE_IDLE;

static float *s_pitch_ring;
static size_t s_ring_write;
static size_t s_ring_valid;
static size_t s_since_last_frame;

static int16_t *s_voice_destination;
static size_t s_voice_capacity;
static size_t s_voice_written;
static size_t s_voice_trim_start;
static size_t s_voice_trim_end;
static size_t s_voice_wait_samples;
static size_t s_voice_silence_samples;
static size_t s_voice_speech_samples;
static unsigned s_voice_active_blocks;
static bool s_voice_started;
static float s_voice_threshold;
static float s_voice_peak_rms;
static esp_err_t s_voice_result = ESP_OK;

static volatile float s_last_rms;
static volatile float s_noise_rms = 0.0015f;
static volatile int32_t s_last_peak;

static float s_dc_previous_x;
static float s_dc_previous_y;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1, z2;
} biquad_t;

static biquad_t s_highpass;
static biquad_t s_lowpass;

static void biquad_design(biquad_t *filter, float cutoff_hz, bool highpass)
{
    const float q = 0.70710678f;
    const float omega = 6.2831853071795864769f * cutoff_hz / APP_MIC_SAMPLE_RATE;
    const float cosine = cosf(omega);
    const float sine = sinf(omega);
    const float alpha = sine / (2.0f * q);
    const float a0 = 1.0f + alpha;

    if (highpass) {
        filter->b0 = ((1.0f + cosine) * 0.5f) / a0;
        filter->b1 = (-(1.0f + cosine)) / a0;
        filter->b2 = ((1.0f + cosine) * 0.5f) / a0;
    } else {
        filter->b0 = ((1.0f - cosine) * 0.5f) / a0;
        filter->b1 = (1.0f - cosine) / a0;
        filter->b2 = ((1.0f - cosine) * 0.5f) / a0;
    }
    filter->a1 = (-2.0f * cosine) / a0;
    filter->a2 = (1.0f - alpha) / a0;
    filter->z1 = 0.0f;
    filter->z2 = 0.0f;
}

static float biquad_process(biquad_t *filter, float input)
{
    float output = filter->b0 * input + filter->z1;
    filter->z1 = filter->b1 * input - filter->a1 * output + filter->z2;
    filter->z2 = filter->b2 * input - filter->a2 * output;
    return output;
}

static int16_t clamp_i16(float value)
{
    if (value > 32767.0f) return 32767;
    if (value < -32768.0f) return -32768;
    return (int16_t)lrintf(value);
}

static int16_t convert_voice_sample(int32_t raw)
{
    float x = (float)(raw >> APP_MIC_RIGHT_SHIFT) * APP_MIC_DIGITAL_GAIN;
    /* WakeNet和科大讯飞使用只去直流的宽带语音PCM，避免3.5 kHz低通削弱语音特征。 */
    float y = x - s_dc_previous_x + 0.995f * s_dc_previous_y;
    s_dc_previous_x = x;
    s_dc_previous_y = y;
    return clamp_i16(y);
}

static int16_t convert_pitch_sample(int16_t voice_sample)
{
    float y = biquad_process(&s_highpass, (float)voice_sample);
    y = biquad_process(&s_lowpass, y);
    return clamp_i16(y);
}

static void make_ordered_pitch_frame(float *destination)
{
    size_t first = s_ring_write;
    size_t first_count = AUDIO_PITCH_FRAME_SAMPLES - first;
    memcpy(destination, s_pitch_ring + first, first_count * sizeof(float));
    if (first > 0) {
        memcpy(destination + first_count, s_pitch_ring, first * sizeof(float));
    }
}

static void publish_pitch_frame(void)
{
    float *frame = NULL;
    if (xQueueReceive(s_free_frames, &frame, 0) != pdTRUE) {
        /* DSP来不及处理时丢弃最老的一帧，采样绝不能阻塞。 */
        if (xQueueReceive(s_ready_frames, &frame, 0) != pdTRUE) return;
    }
    make_ordered_pitch_frame(frame);
    if (xQueueSend(s_ready_frames, &frame, 0) != pdTRUE) {
        xQueueSend(s_free_frames, &frame, 0);
    }
}

static void audio_task(void *argument)
{
    int32_t raw[APP_MIC_BLOCK_SAMPLES];
    int16_t voice_pcm[APP_MIC_BLOCK_SAMPLES];
    int16_t pitch_pcm[APP_MIC_BLOCK_SAMPLES];

    while (true) {
        size_t bytes_read = 0;
        esp_err_t error = i2s_channel_read(
            s_rx_channel,
            raw,
            sizeof(raw),
            &bytes_read,
            portMAX_DELAY
        );
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t block_end_us = esp_timer_get_time();
        size_t count = bytes_read / sizeof(int32_t);
        double sum_square = 0.0;
        double voice_sum_square = 0.0;
        int32_t peak = 0;
        for (size_t index = 0; index < count; ++index) {
            voice_pcm[index] = convert_voice_sample(raw[index]);
            pitch_pcm[index] = convert_pitch_sample(voice_pcm[index]);
            int32_t absolute = pitch_pcm[index] >= 0 ? pitch_pcm[index] : -pitch_pcm[index];
            if (absolute > peak) peak = absolute;
            float normalized = (float)pitch_pcm[index] / 32768.0f;
            float voice_normalized = (float)voice_pcm[index] / 32768.0f;
            sum_square += normalized * normalized;
            voice_sum_square += voice_normalized * voice_normalized;
        }
        float block_rms = count ? sqrtf((float)(sum_square / count)) : 0.0f;
        float voice_block_rms = count ? sqrtf((float)(voice_sum_square / count)) : 0.0f;
        s_last_rms = block_rms;
        s_last_peak = peak;

        /* 只在较安静时缓慢更新噪声底，避免把持续琴声当成底噪。 */
        if (block_rms < fmaxf(0.012f, s_noise_rms * 2.2f)) {
            s_noise_rms = 0.985f * s_noise_rms + 0.015f * block_rms;
        }

        /* WakeNet始终监听同一只INMP441；检测到唤醒词后会自动停止喂入。 */
        wake_word_feed(voice_pcm, count);

        xSemaphoreTake(s_route_mutex, portMAX_DELAY);
        audio_route_t route = s_route;

        if (route == AUDIO_ROUTE_VOICE && s_voice_destination != NULL) {
            size_t remain = s_voice_capacity - s_voice_written;
            size_t copy_count = count < remain ? count : remain;

            if (copy_count > 0) {
                memcpy(
                    s_voice_destination + s_voice_written,
                    voice_pcm,
                    copy_count * sizeof(int16_t)
                );
                s_voice_written += copy_count;
                s_voice_wait_samples += copy_count;
            }

            if (voice_block_rms > s_voice_peak_rms) {
                s_voice_peak_rms = voice_block_rms;
            }

            const bool active =
                voice_block_rms >= s_voice_threshold;
            const size_t start_timeout_samples =
                (size_t)APP_MIC_SAMPLE_RATE
                * APP_VOICE_START_TIMEOUT_MS
                / 1000;
            const size_t end_silence_samples =
                (size_t)APP_MIC_SAMPLE_RATE
                * APP_VOICE_END_SILENCE_MS
                / 1000;
            const size_t minimum_speech_samples =
                (size_t)APP_MIC_SAMPLE_RATE
                * APP_VOICE_MIN_SPEECH_MS
                / 1000;
            const size_t pre_roll_samples =
                (size_t)APP_MIC_SAMPLE_RATE
                * APP_VOICE_PRE_ROLL_MS
                / 1000;
            const size_t post_roll_samples =
                (size_t)APP_MIC_SAMPLE_RATE
                * APP_VOICE_POST_ROLL_MS
                / 1000;

            if (!s_voice_started) {
                if (active) {
                    ++s_voice_active_blocks;
                } else {
                    s_voice_active_blocks = 0;
                }

                if (
                    s_voice_active_blocks
                    >= APP_VOICE_VAD_START_BLOCKS
                ) {
                    s_voice_started = true;
                    size_t onset_backtrack =
                        pre_roll_samples
                        + (
                            size_t
                        )APP_VOICE_VAD_START_BLOCKS
                        * copy_count;
                    s_voice_trim_start =
                        s_voice_written > onset_backtrack
                        ? s_voice_written - onset_backtrack
                        : 0;
                    s_voice_silence_samples = 0;
                    s_voice_speech_samples = 0;
                    ESP_LOGI(
                        TAG,
                        "voice VAD start: rms=%.5f threshold=%.5f",
                        voice_block_rms,
                        s_voice_threshold
                    );
                } else if (
                    s_voice_wait_samples
                    >= start_timeout_samples
                ) {
                    s_voice_result = ESP_ERR_NOT_FOUND;
                    s_voice_trim_start = 0;
                    s_voice_trim_end = 0;
                    s_route = AUDIO_ROUTE_IDLE;
                    xSemaphoreGive(s_voice_done);
                }
            } else {
                if (active) {
                    s_voice_silence_samples = 0;
                    s_voice_speech_samples += copy_count;
                } else if (
                    voice_block_rms
                    < s_voice_threshold
                    * APP_VOICE_VAD_RELEASE_RATIO
                ) {
                    s_voice_silence_samples += copy_count;
                } else {
                    /*
                     * 位于启动/释放阈值之间的声音属于迟滞区，
                     * 不立刻认为语音结束。
                     */
                    s_voice_silence_samples = 0;
                    s_voice_speech_samples += copy_count;
                }

                if (
                    s_voice_speech_samples
                    >= minimum_speech_samples
                    && s_voice_silence_samples
                    >= end_silence_samples
                ) {
                    size_t speech_end =
                        s_voice_written
                        - s_voice_silence_samples;
                    s_voice_trim_end =
                        speech_end + post_roll_samples;
                    if (
                        s_voice_trim_end
                        > s_voice_written
                    ) {
                        s_voice_trim_end =
                            s_voice_written;
                    }
                    s_voice_result = ESP_OK;
                    s_route = AUDIO_ROUTE_IDLE;
                    xSemaphoreGive(s_voice_done);
                }
            }

            if (
                s_route == AUDIO_ROUTE_VOICE
                && s_voice_written
                >= s_voice_capacity
            ) {
                if (s_voice_started) {
                    s_voice_trim_end =
                        s_voice_written;
                    s_voice_result = ESP_OK;
                } else {
                    s_voice_trim_start = 0;
                    s_voice_trim_end = 0;
                    s_voice_result =
                        ESP_ERR_NOT_FOUND;
                }
                s_route = AUDIO_ROUTE_IDLE;
                xSemaphoreGive(s_voice_done);
            }
        } else if (route == AUDIO_ROUTE_PITCH) {
            const size_t window = APP_MIC_SAMPLE_RATE / 100; /* 10 ms */
            for (size_t off=0; off<count; off+=window) {
                size_t n=count-off < window ? count-off : window;
                audio_rhythm_frame_t f={
                    .center_us=block_end_us-(int64_t)(count-off-n/2)*1000000/APP_MIC_SAMPLE_RATE,
                    .rms=onset_pcm_rms(pitch_pcm+off,n), .noise=s_noise_rms};
                f.onset=onset_envelope(&s_onset,f.rms,f.noise,f.center_us);
                if(xQueueSend(s_rhythm_frames,&f,0)!=pdTRUE){
                    audio_rhythm_frame_t old;
                    xQueueReceive(s_rhythm_frames,&old,0);
                    xQueueSend(s_rhythm_frames,&f,0);
                }
            }
            for (size_t index = 0; index < count; ++index) {
                s_pitch_ring[s_ring_write] = (float)pitch_pcm[index] / 32768.0f;
                s_ring_write = (s_ring_write + 1) % AUDIO_PITCH_FRAME_SAMPLES;
                if (s_ring_valid < AUDIO_PITCH_FRAME_SAMPLES) ++s_ring_valid;
                ++s_since_last_frame;
                if (
                    s_ring_valid == AUDIO_PITCH_FRAME_SAMPLES &&
                    s_since_last_frame >= APP_HOP_SIZE
                ) {
                    s_since_last_frame = 0;
                    publish_pitch_frame();
                }
            }
        }
        xSemaphoreGive(s_route_mutex);
    }
}

esp_err_t audio_inmp441_init(void)
{
    biquad_design(&s_highpass, APP_AUDIO_HPF_HZ, true);
    biquad_design(&s_lowpass, APP_AUDIO_LPF_HZ, false);

    s_route_mutex = xSemaphoreCreateMutex();
    s_voice_done = xSemaphoreCreateBinary();
    s_rhythm_frames = xQueueCreate(64, sizeof(audio_rhythm_frame_t));
    onset_reset(&s_onset);
    s_ready_frames = xQueueCreate(2, sizeof(float *));
    s_free_frames = xQueueCreate(2, sizeof(float *));
    ESP_RETURN_ON_FALSE(
        s_route_mutex && s_voice_done && s_ready_frames && s_free_frames && s_rhythm_frames,
        ESP_ERR_NO_MEM,
        TAG,
        "FreeRTOS object allocation failed"
    );

    s_pitch_ring = heap_caps_calloc(
        AUDIO_PITCH_FRAME_SAMPLES,
        sizeof(float),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    ESP_RETURN_ON_FALSE(s_pitch_ring, ESP_ERR_NO_MEM, TAG, "pitch ring allocation failed");

    for (int index = 0; index < 2; ++index) {
        float *frame = heap_caps_malloc(
            AUDIO_PITCH_FRAME_SAMPLES * sizeof(float),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        ESP_RETURN_ON_FALSE(frame, ESP_ERR_NO_MEM, TAG, "pitch frame allocation failed");
        xQueueSend(s_free_frames, &frame, portMAX_DELAY);
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        I2S_NUM_AUTO,
        I2S_ROLE_MASTER
    );
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = APP_MIC_BLOCK_SAMPLES;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, NULL, &s_rx_channel),
        TAG,
        "new I2S channel failed"
    );

    i2s_std_slot_config_t slot_config = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT,
        I2S_SLOT_MODE_MONO
    );
    slot_config.slot_mask = APP_MIC_LEFT_CHANNEL ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(APP_MIC_SAMPLE_RATE),
        .slot_cfg = slot_config,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_I2S_BCLK_GPIO,
            .ws = APP_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = APP_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(s_rx_channel, &standard_config),
        TAG,
        "I2S standard mode init failed"
    );
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "I2S enable failed");

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_task,
        "inmp441_audio",
        6144,
        NULL,
        20,
        NULL,
        0
    );
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "audio task create failed");

    ESP_LOGI(
        TAG,
        "ready: %d Hz, BCLK=%d WS=%d DIN=%d, shift=%d, band=%.0f..%.0f Hz",
        APP_MIC_SAMPLE_RATE,
        APP_I2S_BCLK_GPIO,
        APP_I2S_WS_GPIO,
        APP_I2S_DIN_GPIO,
        APP_MIC_RIGHT_SHIFT,
        APP_AUDIO_HPF_HZ,
        APP_AUDIO_LPF_HZ
    );
    return ESP_OK;
}

void audio_inmp441_set_route(audio_route_t route)
{
    xSemaphoreTake(s_route_mutex, portMAX_DELAY);
    if(s_route != route) {
        onset_reset(&s_onset);
        audio_rhythm_frame_t old;
        while(xQueueReceive(s_rhythm_frames,&old,0)==pdTRUE){}
    }
    s_route = route;
    if (route != AUDIO_ROUTE_PITCH) {
        s_ring_valid = 0;
        s_ring_write = 0;
        s_since_last_frame = 0;
        float *frame = NULL;
        while (xQueueReceive(s_ready_frames, &frame, 0) == pdTRUE) {
            xQueueSend(s_free_frames, &frame, 0);
        }
    }
    xSemaphoreGive(s_route_mutex);
}

esp_err_t audio_inmp441_get_pitch_frame(float *destination, TickType_t timeout)
{
    if (!destination) return ESP_ERR_INVALID_ARG;
    float *frame = NULL;
    if (xQueueReceive(s_ready_frames, &frame, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    memcpy(destination, frame, AUDIO_PITCH_FRAME_SAMPLES * sizeof(float));
    xQueueSend(s_free_frames, &frame, portMAX_DELAY);
    return ESP_OK;
}

static float pcm_rms(
    const int16_t *samples,
    size_t sample_count
)
{
    if (!samples || sample_count == 0) {
        return 0.0f;
    }

    double sum_square = 0.0;
    for (
        size_t index = 0;
        index < sample_count;
        ++index
    ) {
        float value =
            (float)samples[index] / 32768.0f;
        sum_square += value * value;
    }

    return sqrtf(
        (float)(sum_square / sample_count)
    );
}

static float normalize_voice_pcm(
    int16_t *samples,
    size_t sample_count,
    float input_rms
)
{
    if (
        !samples
        || sample_count == 0
        || input_rms <= 1e-6f
    ) {
        return 1.0f;
    }

    int32_t peak = 1;
    for (
        size_t index = 0;
        index < sample_count;
        ++index
    ) {
        int32_t value = samples[index];
        int32_t absolute =
            value >= 0 ? value : -value;
        if (absolute > peak) {
            peak = absolute;
        }
    }

    float rms_gain =
        APP_VOICE_TARGET_RMS / input_rms;
    float peak_gain =
        28000.0f / (float)peak;
    float gain = fminf(
        APP_VOICE_MAX_GAIN,
        fminf(rms_gain, peak_gain)
    );

    if (gain < 1.0f) {
        gain = fmaxf(gain, 0.55f);
    }

    for (
        size_t index = 0;
        index < sample_count;
        ++index
    ) {
        samples[index] = clamp_i16(
            (float)samples[index] * gain
        );
    }

    return gain;
}

esp_err_t audio_inmp441_capture_voice(
    int16_t *destination,
    size_t sample_capacity,
    size_t *samples_written,
    audio_voice_capture_info_t *capture_info,
    TickType_t timeout
)
{
    if (
        !destination
        || sample_capacity == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (samples_written) {
        *samples_written = 0;
    }
    if (capture_info) {
        memset(
            capture_info,
            0,
            sizeof(*capture_info)
        );
    }

    while (
        xSemaphoreTake(s_voice_done, 0)
        == pdTRUE
    ) {}

    xSemaphoreTake(
        s_route_mutex,
        portMAX_DELAY
    );

    s_ring_valid = 0;
    s_ring_write = 0;
    s_since_last_frame = 0;

    float *stale_frame = NULL;
    while (
        xQueueReceive(
            s_ready_frames,
            &stale_frame,
            0
        )
        == pdTRUE
    ) {
        xQueueSend(
            s_free_frames,
            &stale_frame,
            0
        );
    }

    s_voice_destination = destination;
    s_voice_capacity = sample_capacity;
    s_voice_written = 0;
    s_voice_trim_start = 0;
    s_voice_trim_end = 0;
    s_voice_wait_samples = 0;
    s_voice_silence_samples = 0;
    s_voice_speech_samples = 0;
    s_voice_active_blocks = 0;
    s_voice_started = false;
    s_voice_peak_rms = 0.0f;
    s_voice_result = ESP_OK;
    s_voice_threshold = fmaxf(
        APP_VOICE_VAD_MIN_RMS,
        s_noise_rms
        * APP_VOICE_VAD_NOISE_MULT
    );
    s_route = AUDIO_ROUTE_VOICE;

    ESP_LOGI(
        TAG,
        "voice VAD armed: threshold=%.5f noise=%.5f",
        s_voice_threshold,
        s_noise_rms
    );

    xSemaphoreGive(s_route_mutex);

    if (
        xSemaphoreTake(
            s_voice_done,
            timeout
        )
        != pdTRUE
    ) {
        xSemaphoreTake(
            s_route_mutex,
            portMAX_DELAY
        );
        s_route = AUDIO_ROUTE_IDLE;
        s_voice_destination = NULL;
        s_voice_capacity = 0;
        s_voice_written = 0;
        xSemaphoreGive(s_route_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(
        s_route_mutex,
        portMAX_DELAY
    );

    esp_err_t result = s_voice_result;
    size_t trim_start = s_voice_trim_start;
    size_t trim_end = s_voice_trim_end;
    float threshold = s_voice_threshold;
    float peak_rms = s_voice_peak_rms;

    s_voice_destination = NULL;
    s_voice_capacity = 0;

    xSemaphoreGive(s_route_mutex);

    if (
        result != ESP_OK
        || trim_end <= trim_start
    ) {
        ESP_LOGW(
            TAG,
            "voice VAD: no valid speech"
        );
        return ESP_ERR_NOT_FOUND;
    }

    size_t trimmed_count =
        trim_end - trim_start;

    if (trim_start > 0) {
        memmove(
            destination,
            destination + trim_start,
            trimmed_count * sizeof(int16_t)
        );
    }

    float raw_rms = pcm_rms(
        destination,
        trimmed_count
    );
    float gain = normalize_voice_pcm(
        destination,
        trimmed_count,
        raw_rms
    );

    if (samples_written) {
        *samples_written = trimmed_count;
    }

    if (capture_info) {
        capture_info->vad_threshold =
            threshold;
        capture_info->raw_rms = raw_rms;
        capture_info->gain_applied = gain;
        capture_info->duration_ms =
            (uint32_t)(
                trimmed_count * 1000ULL
                / APP_MIC_SAMPLE_RATE
            );
        capture_info->speech_detected =
            true;
    }

    ESP_LOGI(
        TAG,
        "voice capture: %u ms, samples=%u, raw_rms=%.5f, peak_rms=%.5f, gain=%.2f",
        (unsigned)(
            trimmed_count * 1000ULL
            / APP_MIC_SAMPLE_RATE
        ),
        (unsigned)trimmed_count,
        raw_rms,
        peak_rms,
        gain
    );

    return ESP_OK;
}

float audio_inmp441_last_rms(void) { return s_last_rms; }
float audio_inmp441_noise_rms(void) { return s_noise_rms; }
int32_t audio_inmp441_last_peak(void) { return s_last_peak; }

esp_err_t audio_inmp441_get_rhythm_frame(audio_rhythm_frame_t *f, TickType_t timeout)
{ return f && xQueueReceive(s_rhythm_frames,f,timeout)==pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT; }
