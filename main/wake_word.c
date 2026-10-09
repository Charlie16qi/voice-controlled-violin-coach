#include "wake_word.h"

#include <string.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "WAKE_WORD";

static const esp_wn_iface_t *s_wakenet = NULL;
static void *s_model_data = NULL;
static srmodel_list_t *s_models = NULL;
static char *s_model_name = NULL;
static StreamBufferHandle_t s_audio_stream = NULL;
static SemaphoreHandle_t s_detected = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static bool s_enabled = true;
static int s_chunk_samples = 0;

static bool take_wake_arm(void)
{
    bool accepted = false;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    if (s_enabled) {
        s_enabled = false;
        accepted = true;
    }

    xSemaphoreGive(s_state_mutex);
    return accepted;
}

static void wake_detect_task(void *argument)
{
    const size_t chunk_bytes = (size_t)s_chunk_samples * sizeof(int16_t);
    int16_t *chunk = heap_caps_malloc(chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGE(TAG, "wake chunk allocation failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        size_t collected = 0;
        while (collected < chunk_bytes) {
            size_t received = xStreamBufferReceive(
                s_audio_stream,
                (uint8_t *)chunk + collected,
                chunk_bytes - collected,
                portMAX_DELAY
            );
            collected += received;
        }

        /*
         * V3.1:
         * WakeNet持续吃连续音频。忙于处理上一条命令时，
         * 只禁止“发出新的唤醒事件”，不停止detect()。
         */
        int result = s_wakenet->detect(s_model_data, chunk);

        if (result > 0) {
            if (take_wake_arm()) {
                ESP_LOGI(
                    TAG,
                    "wake word detected: id=%d model=%s",
                    result,
                    s_model_name ? s_model_name : "unknown"
                );
                xSemaphoreGive(s_detected);
            } else {
                ESP_LOGD(
                    TAG,
                    "wake detected while command busy; ignored"
                );
            }
        }
    }
}

esp_err_t wake_word_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    s_detected = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_state_mutex && s_detected, ESP_ERR_NO_MEM, TAG, "sync allocation failed");

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models, ESP_FAIL, TAG, "model partition not found; flash the selected ESP-SR model");

    /*
     * ESP-SR model partition may contain multiple model folders.
     * Print all detected models first, then select WakeNet.
     * Some ESP-SR packages require the explicit WakeNet model prefix.
     */
        /*
     * Select WakeNet model from ESP-SR model partition
     */
    s_model_name = esp_srmodel_filter(
        s_models,
        ESP_WN_PREFIX,
        NULL
    );

    if (s_model_name == NULL) {

        ESP_LOGW(
            TAG,
            "WakeNet prefix filter failed, try wn10"
        );

        s_model_name = esp_srmodel_filter(
            s_models,
            "wn10",
            NULL
        );
    }


    ESP_RETURN_ON_FALSE(
        s_model_name,
        ESP_ERR_NOT_FOUND,
        TAG,
        "no WakeNet model found in model partition"
    );


    ESP_LOGI(
        TAG,
        "Selected WakeNet model: %s",
        s_model_name
    );

    ESP_RETURN_ON_FALSE(s_model_name, ESP_ERR_NOT_FOUND, TAG,
                        "no WakeNet model found in model partition");

    ESP_LOGI(TAG, "Selected WakeNet model: %s", s_model_name);

    s_wakenet = (const esp_wn_iface_t *)esp_wn_handle_from_name(s_model_name);
    ESP_RETURN_ON_FALSE(s_wakenet, ESP_ERR_NOT_FOUND, TAG,
                        "WakeNet interface not found: %s", s_model_name);

    s_model_data = s_wakenet->create(s_model_name, DET_MODE_90);
    ESP_RETURN_ON_FALSE(s_model_data, ESP_FAIL, TAG,
                        "WakeNet model create failed: %s", s_model_name);

    s_chunk_samples = s_wakenet->get_samp_chunksize(s_model_data);
    int sample_rate = s_wakenet->get_samp_rate(s_model_data);
    ESP_RETURN_ON_FALSE(sample_rate == APP_MIC_SAMPLE_RATE, ESP_ERR_INVALID_STATE, TAG,
                        "WakeNet rate=%d but microphone rate=%d", sample_rate, APP_MIC_SAMPLE_RATE);
    ESP_RETURN_ON_FALSE(s_chunk_samples > 0, ESP_FAIL, TAG, "invalid WakeNet chunk size");

    size_t stream_bytes = APP_WAKE_STREAM_SAMPLES * sizeof(int16_t);
    size_t chunk_bytes = (size_t)s_chunk_samples * sizeof(int16_t);
    if (stream_bytes < chunk_bytes * 2) stream_bytes = chunk_bytes * 2;
    s_audio_stream = xStreamBufferCreate(stream_bytes, chunk_bytes);
    ESP_RETURN_ON_FALSE(s_audio_stream, ESP_ERR_NO_MEM, TAG, "audio stream allocation failed");

    BaseType_t created = xTaskCreatePinnedToCore(
        wake_detect_task,
        "wakenet_detect",
        12288,
        NULL,
        10,
        NULL,
        1
    );
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "WakeNet task create failed");

    ESP_LOGI(TAG, "ready: model=%s chunk=%d rate=%d", s_model_name, s_chunk_samples, sample_rate);
    return ESP_OK;
}

void wake_word_feed(const int16_t *samples, size_t sample_count)
{
    /*
     * V3.1关键修复：
     * 即使命令正在录音/上云，也继续给WakeNet喂连续音频。
     * s_enabled现在只控制“是否允许触发事件”。
     */
    if (!samples || sample_count == 0 || !s_audio_stream) return;

    const size_t bytes = sample_count * sizeof(int16_t);

    if (xStreamBufferSpacesAvailable(s_audio_stream) < bytes) {
        /* 检测任务来不及处理时丢弃本块，绝不能阻塞I2S采样。 */
        return;
    }

    (void)xStreamBufferSend(
        s_audio_stream,
        samples,
        bytes,
        0
    );
}

esp_err_t wake_word_wait(TickType_t timeout)
{
    if (!s_detected) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(s_detected, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wake_word_trigger(void)
{
    if (!s_detected || !s_state_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!take_wake_arm()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(s_detected);
    ESP_LOGI(TAG, "manual wake trigger accepted");
    return ESP_OK;
}

void wake_word_set_enabled(bool enabled)
{
    if (!s_state_mutex) return;

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_enabled = enabled;
    xSemaphoreGive(s_state_mutex);

    /*
     * 不再reset StreamBuffer。
     * 重新武装时只清掉旧事件，让WakeNet内部时序保持连续。
     */
    if (enabled && s_detected) {
        while (
            xSemaphoreTake(s_detected, 0)
            == pdTRUE
        ) {
        }

        ESP_LOGI(TAG, "wake listener re-armed");
    }
}

const char *wake_word_model_name(void)
{
    return s_model_name ? s_model_name : "not loaded";
}
