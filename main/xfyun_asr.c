#include "xfyun_asr.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_secrets.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "XFYUN";

#define XFYUN_HOST "iat-api.xfyun.cn"
#define XFYUN_PATH "/v2/iat"
#define XFYUN_URL  "wss://iat-api.xfyun.cn/v2/iat"

#define CONNECTED_BIT BIT0
#define FINISHED_BIT  BIT1
#define ERROR_BIT     BIT2

#define ASR_FRAGMENT_CAPACITY 6144
#define ASR_MAX_SEGMENTS      48
#define ASR_SEGMENT_CAPACITY  96

typedef struct {
    EventGroupHandle_t events;

    char *result;
    size_t result_capacity;

    char *fragment;
    size_t fragment_capacity;
    size_t fragment_length;

    char segments[
        ASR_MAX_SEGMENTS
    ][ASR_SEGMENT_CAPACITY];
    bool segment_valid[
        ASR_MAX_SEGMENTS
    ];

    int server_code;
} asr_context_t;

static bool is_placeholder(
    const char *value
)
{
    return (
        !value
        || !value[0]
        || strstr(value, "YOUR_") != NULL
    );
}

esp_err_t xfyun_asr_validate_configuration(void)
{
    if (
        is_placeholder(APP_XFYUN_APP_ID)
        || is_placeholder(APP_XFYUN_API_KEY)
        || is_placeholder(
            APP_XFYUN_API_SECRET
        )
    ) {
        ESP_LOGE(
            TAG,
            "edit main/app_secrets.h first"
        );
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t base64_encode_alloc(
    const unsigned char *source,
    size_t source_length,
    char **output
)
{
    if (!output) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source_length == 0) {
        char *buffer = malloc(1);
        if (!buffer) {
            return ESP_ERR_NO_MEM;
        }
        buffer[0] = '\0';
        *output = buffer;
        return ESP_OK;
    }

    if (!source) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t required = 0;
    int result = mbedtls_base64_encode(
        NULL,
        0,
        &required,
        source,
        source_length
    );

    if (
        result
            != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
        && result != 0
    ) {
        return ESP_FAIL;
    }

    char *buffer = malloc(required + 1);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    result = mbedtls_base64_encode(
        (unsigned char *)buffer,
        required,
        &written,
        source,
        source_length
    );

    if (result != 0) {
        free(buffer);
        return ESP_FAIL;
    }

    buffer[written] = '\0';
    *output = buffer;
    return ESP_OK;
}

static char *url_encode(
    const char *source
)
{
    static const char HEX[] =
        "0123456789ABCDEF";

    size_t length = strlen(source);
    char *output = malloc(
        length * 3 + 1
    );
    if (!output) {
        return NULL;
    }

    char *write = output;

    for (
        size_t index = 0;
        index < length;
        ++index
    ) {
        unsigned char value =
            (unsigned char)source[index];

        if (
            isalnum(value)
            || value == '-'
            || value == '_'
            || value == '.'
            || value == '~'
        ) {
            *write++ = (char)value;
        } else {
            *write++ = '%';
            *write++ = HEX[value >> 4];
            *write++ = HEX[value & 0x0F];
        }
    }

    *write = '\0';
    return output;
}

static esp_err_t build_auth_url(
    char **url
)
{
    time_t now;
    struct tm utc;

    time(&now);
    gmtime_r(&now, &utc);

    char date[64];
    strftime(
        date,
        sizeof(date),
        "%a, %d %b %Y %H:%M:%S GMT",
        &utc
    );

    char signature_origin[256];
    snprintf(
        signature_origin,
        sizeof(signature_origin),
        "host: %s\ndate: %s\nGET %s HTTP/1.1",
        XFYUN_HOST,
        date,
        XFYUN_PATH
    );

    unsigned char digest[32];
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(
            MBEDTLS_MD_SHA256
        );

    if (
        !info
        || mbedtls_md_hmac(
            info,
            (
                const unsigned char *
            )APP_XFYUN_API_SECRET,
            strlen(APP_XFYUN_API_SECRET),
            (
                const unsigned char *
            )signature_origin,
            strlen(signature_origin),
            digest
        )
            != 0
    ) {
        return ESP_FAIL;
    }

    char *signature = NULL;
    ESP_RETURN_ON_ERROR(
        base64_encode_alloc(
            digest,
            sizeof(digest),
            &signature
        ),
        TAG,
        "signature base64 failed"
    );

    char authorization_origin[512];
    snprintf(
        authorization_origin,
        sizeof(authorization_origin),
        "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
        APP_XFYUN_API_KEY,
        signature
    );
    free(signature);

    char *authorization = NULL;
    ESP_RETURN_ON_ERROR(
        base64_encode_alloc(
            (
                const unsigned char *
            )authorization_origin,
            strlen(authorization_origin),
            &authorization
        ),
        TAG,
        "authorization base64 failed"
    );

    char *authorization_encoded =
        url_encode(authorization);
    char *date_encoded =
        url_encode(date);
    char *host_encoded =
        url_encode(XFYUN_HOST);

    free(authorization);

    if (
        !authorization_encoded
        || !date_encoded
        || !host_encoded
    ) {
        free(authorization_encoded);
        free(date_encoded);
        free(host_encoded);
        return ESP_ERR_NO_MEM;
    }

    size_t needed =
        strlen(XFYUN_URL)
        + strlen(authorization_encoded)
        + strlen(date_encoded)
        + strlen(host_encoded)
        + 64;

    *url = malloc(needed);
    if (!*url) {
        free(authorization_encoded);
        free(date_encoded);
        free(host_encoded);
        return ESP_ERR_NO_MEM;
    }

    snprintf(
        *url,
        needed,
        "%s?authorization=%s&date=%s&host=%s",
        XFYUN_URL,
        authorization_encoded,
        date_encoded,
        host_encoded
    );

    free(authorization_encoded);
    free(date_encoded);
    free(host_encoded);
    return ESP_OK;
}

static void clear_segment_range(
    asr_context_t *context,
    int first,
    int last
)
{
    if (!context) {
        return;
    }

    if (first < 0) {
        first = 0;
    }
    if (last >= ASR_MAX_SEGMENTS) {
        last = ASR_MAX_SEGMENTS - 1;
    }

    for (
        int index = first;
        index <= last;
        ++index
    ) {
        context->segment_valid[index] =
            false;
        context->segments[index][0] =
            '\0';
    }
}

static void rebuild_result(
    asr_context_t *context
)
{
    if (
        !context
        || !context->result
        || context->result_capacity == 0
    ) {
        return;
    }

    context->result[0] = '\0';
    size_t used = 0;

    for (
        int index = 0;
        index < ASR_MAX_SEGMENTS;
        ++index
    ) {
        if (!context->segment_valid[index]) {
            continue;
        }

        const char *piece =
            context->segments[index];
        size_t piece_length =
            strlen(piece);

        if (
            used + piece_length + 1
            > context->result_capacity
        ) {
            break;
        }

        memcpy(
            context->result + used,
            piece,
            piece_length
        );
        used += piece_length;
        context->result[used] = '\0';
    }
}

static void build_piece_text(
    cJSON *word_segments,
    char *destination,
    size_t capacity
)
{
    if (
        !destination
        || capacity == 0
    ) {
        return;
    }

    destination[0] = '\0';
    size_t used = 0;

    if (!cJSON_IsArray(word_segments)) {
        return;
    }

    cJSON *segment = NULL;
    cJSON_ArrayForEach(
        segment,
        word_segments
    ) {
        cJSON *choices =
            cJSON_GetObjectItem(
                segment,
                "cw"
            );
        cJSON *first =
            cJSON_IsArray(choices)
            ? cJSON_GetArrayItem(
                choices,
                0
            )
            : NULL;
        const char *word =
            first
            ? cJSON_GetStringValue(
                cJSON_GetObjectItem(
                    first,
                    "w"
                )
            )
            : NULL;

        if (!word || !word[0]) {
            continue;
        }

        size_t length = strlen(word);
        if (used + length + 1 > capacity) {
            break;
        }

        memcpy(
            destination + used,
            word,
            length
        );
        used += length;
        destination[used] = '\0';
    }
}

static void parse_message(
    asr_context_t *context,
    const char *message
)
{
    cJSON *root = cJSON_Parse(message);
    if (!root) {
        ESP_LOGW(TAG, "invalid JSON response");
        return;
    }

    cJSON *code =
        cJSON_GetObjectItem(root, "code");

    if (
        cJSON_IsNumber(code)
        && code->valueint != 0
    ) {
        context->server_code =
            code->valueint;

        const char *message_text =
            cJSON_GetStringValue(
                cJSON_GetObjectItem(
                    root,
                    "message"
                )
            );

        ESP_LOGE(
            TAG,
            "server error %d: %s",
            code->valueint,
            message_text
                ? message_text
                : "unknown"
        );

        xEventGroupSetBits(
            context->events,
            ERROR_BIT
        );

        cJSON_Delete(root);
        return;
    }

    cJSON *data =
        cJSON_GetObjectItem(root, "data");
    cJSON *result =
        data
        ? cJSON_GetObjectItem(
            data,
            "result"
        )
        : NULL;

    if (result) {
        cJSON *sn_item =
            cJSON_GetObjectItem(
                result,
                "sn"
            );
        int sn =
            cJSON_IsNumber(sn_item)
            ? sn_item->valueint
            : 0;

        cJSON *pgs_item =
            cJSON_GetObjectItem(
                result,
                "pgs"
            );
        const char *pgs =
            cJSON_GetStringValue(
                pgs_item
            );

        if (
            pgs
            && strcmp(pgs, "rpl") == 0
        ) {
            cJSON *range =
                cJSON_GetObjectItem(
                    result,
                    "rg"
                );

            if (
                cJSON_IsArray(range)
                && cJSON_GetArraySize(
                    range
                )
                    >= 2
            ) {
                cJSON *first =
                    cJSON_GetArrayItem(
                        range,
                        0
                    );
                cJSON *last =
                    cJSON_GetArrayItem(
                        range,
                        1
                    );

                if (
                    cJSON_IsNumber(first)
                    && cJSON_IsNumber(last)
                ) {
                    clear_segment_range(
                        context,
                        first->valueint,
                        last->valueint
                    );
                }
            }
        }

        if (
            sn >= 0
            && sn < ASR_MAX_SEGMENTS
        ) {
            char piece[
                ASR_SEGMENT_CAPACITY
            ];
            build_piece_text(
                cJSON_GetObjectItem(
                    result,
                    "ws"
                ),
                piece,
                sizeof(piece)
            );

            strlcpy(
                context->segments[sn],
                piece,
                sizeof(
                    context->segments[sn]
                )
            );
            context->segment_valid[sn] =
                piece[0] != '\0';
            rebuild_result(context);

            ESP_LOGI(
                TAG,
                "partial sn=%d pgs=%s text=%s",
                sn,
                pgs ? pgs : "-",
                context->result
            );
        }
    }

    cJSON *status =
        data
        ? cJSON_GetObjectItem(
            data,
            "status"
        )
        : NULL;

    if (
        cJSON_IsNumber(status)
        && status->valueint == 2
    ) {
        rebuild_result(context);
        xEventGroupSetBits(
            context->events,
            FINISHED_BIT
        );
    }

    cJSON_Delete(root);
}

static void websocket_event(
    void *handler_argument,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data
)
{
    (void)base;

    asr_context_t *context =
        handler_argument;
    esp_websocket_event_data_t *event =
        event_data;

    if (
        event_id
        == WEBSOCKET_EVENT_CONNECTED
    ) {
        ESP_LOGI(TAG, "websocket connected");
        xEventGroupSetBits(
            context->events,
            CONNECTED_BIT
        );
        return;
    }

    if (
        event_id == WEBSOCKET_EVENT_DATA
        && event
        && event->op_code == 0x1
    ) {
        if (event->payload_offset == 0) {
            context->fragment_length = 0;
        }

        size_t available =
            context->fragment_capacity
            - context->fragment_length
            - 1;
        size_t copy =
            event->data_len < available
            ? event->data_len
            : available;

        memcpy(
            context->fragment
                + context->fragment_length,
            event->data_ptr,
            copy
        );

        context->fragment_length += copy;
        context->fragment[
            context->fragment_length
        ] = '\0';

        if (
            event->payload_offset
                + event->data_len
            >= event->payload_len
        ) {
            parse_message(
                context,
                context->fragment
            );
        }

        return;
    }

    if (
        event_id == WEBSOCKET_EVENT_ERROR
    ) {
        ESP_LOGE(TAG, "websocket error");
        xEventGroupSetBits(
            context->events,
            ERROR_BIT
        );
    }
}

static esp_err_t send_audio_frame(
    esp_websocket_client_handle_t client,
    const uint8_t *audio,
    size_t length,
    int status
)
{
    char *audio_base64 = NULL;

    ESP_RETURN_ON_ERROR(
        base64_encode_alloc(
            audio,
            length,
            &audio_base64
        ),
        TAG,
        "audio base64 failed"
    );

    size_t capacity =
        strlen(audio_base64) + 896;
    char *packet = malloc(capacity);

    if (!packet) {
        free(audio_base64);
        return ESP_ERR_NO_MEM;
    }

    if (status == 0) {
        snprintf(
            packet,
            capacity,
            "{\"common\":{\"app_id\":\"%s\"},"
            "\"business\":{"
            "\"language\":\"zh_cn\","
            "\"domain\":\"iat\","
            "\"accent\":\"mandarin\","
            "\"vad_eos\":650,"
            "\"dwa\":\"wpgs\","
            "\"ptt\":0,"
            "\"nunum\":1"
            "},"
            "\"data\":{"
            "\"status\":0,"
            "\"format\":\"audio/L16;rate=16000\","
            "\"encoding\":\"raw\","
            "\"audio\":\"%s\""
            "}}",
            APP_XFYUN_APP_ID,
            audio_base64
        );
    } else {
        snprintf(
            packet,
            capacity,
            "{\"data\":{"
            "\"status\":%d,"
            "\"format\":\"audio/L16;rate=16000\","
            "\"encoding\":\"raw\","
            "\"audio\":\"%s\""
            "}}",
            status,
            audio_base64
        );
    }

    free(audio_base64);

    int sent =
        esp_websocket_client_send_text(
            client,
            packet,
            strlen(packet),
            pdMS_TO_TICKS(4000)
        );

    free(packet);
    return sent >= 0
        ? ESP_OK
        : ESP_FAIL;
}

esp_err_t xfyun_asr_transcribe(
    const int16_t *pcm,
    size_t sample_count,
    char *result_text,
    size_t result_capacity
)
{
    ESP_RETURN_ON_ERROR(
        xfyun_asr_validate_configuration(),
        TAG,
        "configuration invalid"
    );

    if (
        !pcm
        || sample_count == 0
        || !result_text
        || result_capacity < 2
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    result_text[0] = '\0';

    int64_t started_us =
        esp_timer_get_time();

    char *url = NULL;
    ESP_RETURN_ON_ERROR(
        build_auth_url(&url),
        TAG,
        "auth URL failed"
    );

    asr_context_t context = {
        .events = xEventGroupCreate(),
        .result = result_text,
        .result_capacity = result_capacity,
        .fragment_capacity =
            ASR_FRAGMENT_CAPACITY,
    };

    context.fragment = malloc(
        context.fragment_capacity
    );

    if (
        !context.events
        || !context.fragment
    ) {
        free(url);
        free(context.fragment);
        if (context.events) {
            vEventGroupDelete(
                context.events
            );
        }
        return ESP_ERR_NO_MEM;
    }

    esp_websocket_client_config_t config = {
        .uri = url,
        .buffer_size = 6144,
        .task_stack = 9216,
        .network_timeout_ms = 9000,
        .disable_auto_reconnect = true,
        .crt_bundle_attach =
            esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t client =
        esp_websocket_client_init(
            &config
        );

    if (!client) {
        free(url);
        free(context.fragment);
        vEventGroupDelete(
            context.events
        );
        return ESP_FAIL;
    }

    esp_websocket_register_events(
        client,
        WEBSOCKET_EVENT_ANY,
        websocket_event,
        &context
    );

    bool client_started = false;
    esp_err_t final_result = ESP_FAIL;

    if (
        esp_websocket_client_start(
            client
        )
        != ESP_OK
    ) {
        ESP_LOGE(
            TAG,
            "websocket start failed"
        );
        goto cleanup;
    }

    client_started = true;

    EventBits_t bits =
        xEventGroupWaitBits(
            context.events,
            CONNECTED_BIT | ERROR_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(9000)
        );

    if (
        !(bits & CONNECTED_BIT)
        || (bits & ERROR_BIT)
    ) {
        ESP_LOGE(
            TAG,
            "websocket connection failed"
        );
        goto cleanup;
    }

    const uint8_t *bytes =
        (const uint8_t *)pcm;
    size_t byte_count =
        sample_count * sizeof(int16_t);
    const size_t chunk_size = 1280;
    size_t offset = 0;
    int status = 0;

    ESP_LOGI(
        TAG,
        "uploading %u ms audio",
        (unsigned)(
            sample_count * 1000ULL
            / 16000
        )
    );

    while (offset < byte_count) {
        size_t length =
            byte_count - offset;

        if (length > chunk_size) {
            length = chunk_size;
        }

        if (
            send_audio_frame(
                client,
                bytes + offset,
                length,
                status
            )
            != ESP_OK
        ) {
            goto cleanup;
        }

        status = 1;
        offset += length;

        /*
         * 1280字节 = 40ms的16k/16bit/单声道PCM。
         * 按接口建议的实时节奏发送。
         */
        vTaskDelay(
            pdMS_TO_TICKS(40)
        );
    }

    if (
        send_audio_frame(
            client,
            (const uint8_t *)"",
            0,
            2
        )
        != ESP_OK
    ) {
        goto cleanup;
    }

    bits = xEventGroupWaitBits(
        context.events,
        FINISHED_BIT | ERROR_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(9000)
    );

    if (
        (bits & FINISHED_BIT)
        && !(bits & ERROR_BIT)
        && result_text[0] != '\0'
    ) {
        final_result = ESP_OK;
    } else {
        ESP_LOGE(
            TAG,
            "ASR did not return final text"
        );
    }

cleanup:
    if (client_started) {
        esp_websocket_client_stop(
            client
        );
    }

    esp_websocket_client_destroy(
        client
    );
    free(url);
    free(context.fragment);
    vEventGroupDelete(
        context.events
    );

    ESP_LOGI(
        TAG,
        "ASR total latency: %lld ms, result=%s",
        (
            long long
        )(
            (
                esp_timer_get_time()
                - started_us
            )
            / 1000
        ),
        result_text[0]
            ? result_text
            : "<empty>"
    );

    return final_result;
}
