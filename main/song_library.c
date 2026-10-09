#include "song_library.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_secrets.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "SONG_LIBRARY";

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} response_buffer_t;

static bool is_placeholder_url(void)
{
    return (
        !APP_SONG_LIBRARY_BASE_URL[0]
        || strstr(
            APP_SONG_LIBRARY_BASE_URL,
            "YOUR_"
        ) != NULL
        || strstr(
            APP_SONG_LIBRARY_BASE_URL,
            "127.0.0.1"
        ) != NULL
    );
}


static uint8_t auto_string_id_for_note(const char *note)
{
    if (!note || strcmp(note, "REST") == 0) return 0;
    int midi = note_to_midi(note);
    if (midi < 0) return 0;
    if (midi >= 76) return 1; /* E5 */
    if (midi >= 69) return 2; /* A4 */
    if (midi >= 62) return 3; /* D4 */
    return 4;                 /* G3 */
}

static int8_t infer_finger_for_note(const char *note, uint8_t string_id)
{
    if (!note || strcmp(note, "REST") == 0 || string_id < 1 || string_id > 4) return -1;
    static const int OPEN_MIDI[5] = {0, 76, 69, 62, 55};
    int midi = note_to_midi(note);
    int delta = midi - OPEN_MIDI[string_id];
    if (delta == 0) return 0;
    /* First-position pedagogical approximation; MusicXML fingering overrides it. */
    if (delta == 1 || delta == 2) return 1;
    if (delta == 3 || delta == 4) return 2;
    if (delta == 5) return 3;
    if (delta == 6 || delta == 7) return 4;
    return -1;
}

static void clear_song(song_t *song)
{
    if (!song) return;
    memset(song, 0, sizeof(*song));
}

void song_library_free(song_t *song)
{
    if (!song) return;
    free(song->events);
    clear_song(song);
}

static esp_err_t copy_events(
    song_t *song,
    const char *title,
    const char *id,
    float tempo_bpm,
    float tolerance_cents,
    uint32_t hold_ms,
    const char *const *notes,
    const float *beats,
    size_t count
)
{
    if (
        !song
        || !notes
        || count == 0
        || count > APP_MAX_SONG_NOTES
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    song_event_t *events = calloc(
        count,
        sizeof(song_event_t)
    );
    if (!events) return ESP_ERR_NO_MEM;

    uint16_t virtual_measure = 1;
    float virtual_beat = 1.0f;

    for (
        size_t index = 0;
        index < count;
        ++index
    ) {
        if (
            strcmp(notes[index], "REST") != 0
            && !note_is_supported(notes[index])
        ) {
            free(events);
            return ESP_ERR_INVALID_ARG;
        }

        strlcpy(
            events[index].note,
            notes[index],
            sizeof(events[index].note)
        );
        events[index].beats =
            beats ? beats[index] : 1.0f;
        events[index].string_id = auto_string_id_for_note(events[index].note);
        events[index].finger = infer_finger_for_note(events[index].note, events[index].string_id);
        events[index].measure = virtual_measure;
        events[index].beat = virtual_beat;

        virtual_beat += events[index].beats;
        if (virtual_beat > 4.0001f) {
            ++virtual_measure;
            virtual_beat = 1.0f;
        }
    }

    clear_song(song);
    song->events = events;
    song->count = count;
    song->time_num = song->time_den = 4;
    song->tempo_bpm =
        tempo_bpm > 1.0f
        ? tempo_bpm
        : 90.0f;
    song->tolerance_cents =
        tolerance_cents > 1.0f
        ? tolerance_cents
        : APP_MELODY_ACCEPT_CENTS;
    song->hold_ms =
        hold_ms > 0
        ? hold_ms
        : APP_MELODY_HOLD_MS;

    strlcpy(song->title, title, sizeof(song->title));
    strlcpy(song->id, id, sizeof(song->id));

    return ESP_OK;
}

static esp_err_t load_builtin(
    const char *query,
    song_t *song
)
{
    if (!query || !song) return ESP_ERR_INVALID_ARG;

    if (
        strstr(query, "小星星")
        || strstr(query, "小行星")
        || strstr(query, "小猩猩")
        || strstr(query, "亮晶晶")
    ) {
        static const char *NOTES[] = {
            "C4","C4","G4","G4","A4","A4","G4",
            "F4","F4","E4","E4","D4","D4","C4",
            "G4","G4","F4","F4","E4","E4","D4",
            "G4","G4","F4","F4","E4","E4","D4",
            "C4","C4","G4","G4","A4","A4","G4",
            "F4","F4","E4","E4","D4","D4","C4"
        };
        static const float BEATS[] = {
            1,1,1,1,1,1,2,
            1,1,1,1,1,1,2,
            1,1,1,1,1,1,2,
            1,1,1,1,1,1,2,
            1,1,1,1,1,1,2,
            1,1,1,1,1,1,2
        };

        return copy_events(
            song,
            "小星星",
            "builtin-twinkle",
            90.0f,
            30.0f,
            320,
            NOTES,
            BEATS,
            sizeof(NOTES) / sizeof(NOTES[0])
        );
    }

    if (strstr(query, "找朋友")) {
        /*
         * 示例版本，1=G，按公开简谱预览转写：https://www.ccguitar.cn/cchtml/9988828.htm
         * 用户可在网页曲库中上传自己采用的版本覆盖/扩展。
         */
        static const char *NOTES[] = {
            "D5","D5","D5","D5",
            "D5","E5","D5",
            "D5","G5","F#5","E5",
            "D5","E5","D5",
            "D5","D5","B4","B4",
            "D5","D5","B4",
            "A4","C5","B4","A4",
            "G4","A4","G4"
        };

        return copy_events(
            song,
            "找朋友",
            "builtin-find-friend",
            100.0f,
            30.0f,
            300,
            NOTES,
            NULL,
            sizeof(NOTES) / sizeof(NOTES[0])
        );
    }

    return ESP_ERR_NOT_FOUND;
}

static char *url_encode(const char *source)
{
    if (!source) return NULL;

    static const char HEX[] =
        "0123456789ABCDEF";

    size_t length = strlen(source);
    char *output = malloc(length * 3 + 1);
    if (!output) return NULL;

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

static esp_err_t http_event(
    esp_http_client_event_t *event
)
{
    if (
        !event
        || !event->user_data
    ) {
        return ESP_OK;
    }

    response_buffer_t *buffer =
        (response_buffer_t *)event->user_data;

    if (
        event->event_id == HTTP_EVENT_ON_DATA
        && event->data
        && event->data_len > 0
    ) {
        size_t available =
            buffer->capacity
            - buffer->length
            - 1;

        size_t copy =
            (size_t)event->data_len
                < available
            ? (size_t)event->data_len
            : available;

        if (copy > 0) {
            memcpy(
                buffer->data + buffer->length,
                event->data,
                copy
            );
            buffer->length += copy;
            buffer->data[buffer->length] = '\0';
        }
    }

    return ESP_OK;
}

static esp_err_t parse_remote_song(
    const char *json_text,
    song_t *song
)
{
    if (!json_text || !song) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_text);
    if (!root) return ESP_FAIL;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *song_json =
        cJSON_GetObjectItem(root, "song");

    if (
        !cJSON_IsTrue(ok)
        || !cJSON_IsObject(song_json)
    ) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    const char *title = cJSON_GetStringValue(
        cJSON_GetObjectItem(song_json, "title")
    );
    const char *id = cJSON_GetStringValue(
        cJSON_GetObjectItem(song_json, "slug")
    );

    cJSON *num_item = cJSON_GetObjectItem(song_json,"time_num");
    cJSON *den_item = cJSON_GetObjectItem(song_json,"time_den");
    unsigned num = cJSON_IsNumber(num_item) ? num_item->valueint : 4;
    unsigned den = cJSON_IsNumber(den_item) ? den_item->valueint : 4;
    if (num < 1 || num > 12 || (den!=2 && den!=4 && den!=8 && den!=16)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    float measure_quarters = (float)num * 4.0f / den;
    cJSON *tempo_item =
        cJSON_GetObjectItem(song_json, "tempo_bpm");
    cJSON *tolerance_item =
        cJSON_GetObjectItem(song_json, "tolerance_cents");
    cJSON *hold_item =
        cJSON_GetObjectItem(song_json, "hold_ms");
    cJSON *notes =
        cJSON_GetObjectItem(song_json, "notes");

    if (
        !title
        || !id
        || !cJSON_IsArray(notes)
    ) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (!cJSON_IsNumber(tempo_item) || !isfinite(tempo_item->valuedouble) || tempo_item->valuedouble < 20 || tempo_item->valuedouble > 300 || strlen(id)>=sizeof(song->id) || strlen(title)>=sizeof(song->title)) {
        cJSON_Delete(root); return ESP_ERR_INVALID_ARG;
    }
    int count = cJSON_GetArraySize(notes);
    if (
        count <= 0
        || count > APP_MAX_SONG_NOTES
    ) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    song_event_t *events = calloc(
        (size_t)count,
        sizeof(song_event_t)
    );
    if (!events) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (int index = 0; index < count; ++index) {
        cJSON *item =
            cJSON_GetArrayItem(notes, index);
        const char *raw_note =
            item
            ? cJSON_GetStringValue(
                cJSON_GetObjectItem(item, "note")
            )
            : NULL;

        cJSON *beats_item =
            item
            ? cJSON_GetObjectItem(item, "beats")
            : NULL;
        cJSON *string_item = item ? cJSON_GetObjectItem(item, "string_id") : NULL;
        cJSON *finger_item = item ? cJSON_GetObjectItem(item, "finger") : NULL;
        cJSON *measure_item = item ? cJSON_GetObjectItem(item, "measure") : NULL;
        cJSON *beat_item = item ? cJSON_GetObjectItem(item, "beat") : NULL;

        if (!raw_note || !cJSON_IsNumber(beats_item) || !isfinite(beats_item->valuedouble) || beats_item->valuedouble<=0 || beats_item->valuedouble>16) {
            free(events);
            cJSON_Delete(root);
            return ESP_FAIL;
        }

        if (strcmp(raw_note, "REST") == 0) {
            strlcpy(
                events[index].note,
                "REST",
                sizeof(events[index].note)
            );
        } else {
            char canonical[NOTE_NAME_CAPACITY];
            if (
                !note_parse(
                    raw_note,
                    canonical,
                    sizeof(canonical)
                )
                || !note_is_supported(canonical)
            ) {
                free(events);
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }

            strlcpy(
                events[index].note,
                canonical,
                sizeof(events[index].note)
            );
        }

        events[index].beats =
            cJSON_IsNumber(beats_item)
            && beats_item->valuedouble > 0.0
            ? (float)beats_item->valuedouble
            : 1.0f;

        int parsed_string = cJSON_IsNumber(string_item) ? string_item->valueint : 0;
        events[index].string_id =
            parsed_string >= 1 && parsed_string <= 4
            ? (uint8_t)parsed_string
            : auto_string_id_for_note(events[index].note);
        events[index].finger = cJSON_IsNumber(finger_item)
            ? (int8_t)finger_item->valueint
            : infer_finger_for_note(events[index].note, events[index].string_id);
        events[index].measure = cJSON_IsNumber(measure_item) && measure_item->valueint > 0
            ? (uint16_t)measure_item->valueint
            : 0;
        events[index].beat = cJSON_IsNumber(beat_item) && beat_item->valuedouble > 0.0
            ? (float)beat_item->valuedouble
            : 0.0f;
    }

    /* Fill missing measure/beat metadata for legacy CSV/built-in songs. */
    uint16_t fallback_measure = 1;
    float fallback_beat = 1.0f;
    for (int index = 0; index < count; ++index) {
        if (events[index].measure == 0) events[index].measure = fallback_measure;
        if (events[index].beat <= 0.0f) events[index].beat = fallback_beat;
        fallback_measure = events[index].measure;
        fallback_beat = events[index].beat + events[index].beats;
        while (fallback_beat >= measure_quarters + 0.9999f) {
            ++fallback_measure;
            fallback_beat -= measure_quarters;
        }
    }

    clear_song(song);
    song->events = events;
    song->count = (size_t)count;
    song->time_num = num;
    song->time_den = den;
    song->tempo_bpm =
        cJSON_IsNumber(tempo_item)
        ? (float)tempo_item->valuedouble
        : 90.0f;
    song->tolerance_cents =
        cJSON_IsNumber(tolerance_item)
        ? (float)tolerance_item->valuedouble
        : APP_MELODY_ACCEPT_CENTS;
    song->hold_ms =
        cJSON_IsNumber(hold_item)
        ? (uint32_t)hold_item->valueint
        : APP_MELODY_HOLD_MS;

    strlcpy(song->title, title, sizeof(song->title));
    strlcpy(song->id, id, sizeof(song->id));

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t fetch_remote(
    const char *query,
    song_t *song
)
{
    if (
        !query
        || !song
        || is_placeholder_url()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    char *encoded = url_encode(query);
    if (!encoded) return ESP_ERR_NO_MEM;

    size_t url_capacity =
        strlen(APP_SONG_LIBRARY_BASE_URL)
        + strlen(encoded)
        + 64;

    char *url = malloc(url_capacity);
    if (!url) {
        free(encoded);
        return ESP_ERR_NO_MEM;
    }

    snprintf(
        url,
        url_capacity,
        "%s/api/songs/resolve?strict=1&q=%s",
        APP_SONG_LIBRARY_BASE_URL,
        encoded
    );
    free(encoded);

    char *response = calloc(1, 32768);
    if (!response) {
        free(url);
        return ESP_ERR_NO_MEM;
    }

    response_buffer_t buffer = {
        .data = response,
        .capacity = 32768,
        .length = 0,
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 6000,
        .event_handler = http_event,
        .user_data = &buffer,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (!client) {
        free(response);
        free(url);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "query remote library: %s", query);

    esp_err_t error =
        esp_http_client_perform(client);

    int status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(url);

    if (
        error != ESP_OK
        || status != 200
    ) {
        ESP_LOGW(
            TAG,
            "remote library failed: err=%s http=%d",
            esp_err_to_name(error),
            status
        );
        free(response);
        return ESP_FAIL;
    }

    error = parse_remote_song(
        response,
        song
    );

    free(response);
    return error;
}

/* Guarded selection must use the user's real server score. HTTP failure is
 * reported, rather than silently substituting a differently arranged builtin. */
esp_err_t song_library_fetch_strict(const char *query, song_t *song)
{
    if (!query || !song) return ESP_ERR_INVALID_ARG;
    song_library_free(song);
    return fetch_remote(query, song);
}

esp_err_t song_library_fetch(
    const char *query,
    song_t *song
)
{
    if (!query || !song) return ESP_ERR_INVALID_ARG;

    song_library_free(song);

    /*
     * 新增曲目优先从网页曲库取；
     * 小星星/找朋友在网页不可用时还有本地兜底。
     */
    esp_err_t error = fetch_remote(query, song);
    if (error == ESP_OK) {
        ESP_LOGI(
            TAG,
            "loaded remote song: %s (%u notes)",
            song->title,
            (unsigned)song->count
        );
        return ESP_OK;
    }

    error = load_builtin(query, song);
    if (error == ESP_OK) {
        ESP_LOGW(
            TAG,
            "using built-in fallback: %s",
            song->title
        );
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

uint32_t song_event_duration_ms(
    const song_t *song,
    const song_event_t *event
)
{
    if (!song || !event) return APP_MELODY_HOLD_MS;

    float tempo =
        song->tempo_bpm > 1.0f
        ? song->tempo_bpm
        : 90.0f;

    float beats = event->beats > 0.0f ? event->beats : 1.0f;
    float duration_ms = (60000.0f / tempo) * beats;

    if (duration_ms < (float)APP_RHYTHM_FRAME_MS) {
        duration_ms = (float)APP_RHYTHM_FRAME_MS;
    }
    if (duration_ms > 60000.0f) {
        duration_ms = 60000.0f;
    }

    return (uint32_t)lrintf(duration_ms);
}

uint32_t song_event_required_ms(
    const song_t *song,
    const song_event_t *event
)
{
    if (!song || !event) return APP_MELODY_HOLD_MS;

    float tempo =
        song->tempo_bpm > 1.0f
        ? song->tempo_bpm
        : 90.0f;

    float beat_ms = 60000.0f / tempo;

    if (strcmp(event->note, "REST") == 0) {
        float rest_ms =
            fmaxf(120.0f, beat_ms * event->beats);
        return (uint32_t)lrintf(rest_ms);
    }

    float musical_hold =
        beat_ms * event->beats * 0.35f;

    uint32_t minimum =
        song->hold_ms > 0
        ? song->hold_ms
        : APP_MELODY_HOLD_MS;

    uint32_t derived =
        (uint32_t)lrintf(
            fminf(
                1200.0f,
                fmaxf(220.0f, musical_hold)
            )
        );

    uint32_t required =
        derived > minimum
        ? derived
        : minimum;

    if (
        required
        > APP_MELODY_MAX_HOLD_MS
    ) {
        required =
            APP_MELODY_MAX_HOLD_MS;
    }

    return required;
}
