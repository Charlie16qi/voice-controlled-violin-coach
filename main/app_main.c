#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_secrets.h"
#include "audio_inmp441.h"
#include "display_st7789.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "rhythm_trainer.h"
#include "follow_trainer.h"
#include "hps_pitch.h"
#include "intent.h"
#include "led_bar.h"
#include "note_utils.h"
#include "song_library.h"
#include "wake_word.h"
#include "wifi_time.h"
#include "xfyun_asr.h"

static void vc_emit_target_locked(void);

static void vc_emit_rhythm_note_locked(
    size_t index,
    const song_event_t *event,
    uint32_t beat,
    uint32_t elapsed,
    const char *status
);

static void vc_emit_song_definition_locked(void);

static void vc_emit_state_locked(void);

static void vc_emit_song_action_locked(
    const char *action
);

static void vc_emit_asr_text(
    const char *text
);


static const char *TAG = "VIOLIN_V3";
#define VC_FIRMWARE_BUILD "V10.8.2-SINGLE"

typedef enum {
    STATE_HOME = 0,
    STATE_SINGLE,
    STATE_MELODY,
    STATE_PAUSED,
    STATE_VOICE,
    STATE_CLOUD,
} app_state_t;

typedef enum {
    PRACTICE_PITCH = 0,
    PRACTICE_RHYTHM,
    PRACTICE_FULL,
    PRACTICE_FOLLOW,
} practice_mode_t;

typedef struct {
    app_state_t state;
    app_state_t resume_state;
    char target_note[NOTE_NAME_CAPACITY];
    float target_hz;
    int target_string_id; /* 1=E,2=A,3=D,4=G,0=auto/unspecified */
    size_t melody_index;
    bool melody_release_required;
    uint32_t melody_stable_ms;
    uint32_t melody_release_ms;

    /* Fixed score clock with explicit phrase boundaries (V10.6). */
    practice_mode_t practice_mode;
    bool metronome_enabled;
    uint32_t rhythm_tolerance_ms;
    /* Mirrored live timing and per-note feedback. */
    bool rhythm_started;
    uint32_t rhythm_onset_candidate_ms;
    uint32_t rhythm_boundary_candidate_ms;
    uint32_t rhythm_elapsed_ms;
    int64_t rhythm_last_frame_us;
    float rhythm_elapsed_beats;
    float song_clock_beats;
    int32_t rhythm_onset_ms;
    uint32_t rhythm_sounding_ms;
    uint32_t rhythm_correct_ms;
    float rhythm_cents_sum;
    float rhythm_abs_cents_sum;
    float rhythm_cents_sq_sum;
    uint32_t rhythm_pitch_samples;
} app_context_t;

static app_context_t s_app;
static song_t s_song;
static float s_song_original_tempo = 90.0f;
static SemaphoreHandle_t s_app_mutex;
static SemaphoreHandle_t s_telemetry_mutex;
static rhythm_trainer_t s_trainer;
static follow_trainer_t s_follow;
static bool s_follow_pitch_ok=false;
static int64_t s_follow_pitch_us=0;
static size_t s_follow_pitch_index=0;
static void follow_begin_locked(bool count_in);
static bool timed_mode(void){return s_app.practice_mode==PRACTICE_RHYTHM || s_app.practice_mode==PRACTICE_FULL;}
static int s_rhythm_offset_ms=0;
static uint32_t s_phrase_run=0;
/* Every user control invalidates older, still-running recognition/load jobs. */
static uint32_t s_control_generation = 0;
static uint32_t s_loading_generation = 0;
static app_state_t s_loading_restore_state = STATE_HOME;
typedef struct {
    uint32_t generation;
    app_state_t restore_state;
    char query[96];
} song_request_t;
static QueueHandle_t s_song_requests;
static void phrase_begin_locked(size_t first);
static void show_current_state(void);
static void phrase_command_locked(intent_kind_t kind);
static void vc_emit_trainer_fields_locked(void);
static void vc_emit_song_request_locked(const song_request_t *r, const char *phase, const char *message);


/*
 * Voice navigation guard.
 * A short ASR/WakeNet echo can otherwise execute NEXT/PREVIOUS twice.
 * Web/keyboard commands are not throttled; only cloud voice navigation is.
 */
static intent_kind_t s_last_voice_nav_kind = INTENT_UNKNOWN;
static TickType_t s_last_voice_nav_tick = 0;
#define APP_VOICE_NAV_DEBOUNCE_MS 1100

/*
 * V10.4.4 rhythm clock:
 * - actual monotonic elapsed time instead of assuming every HPS/YIN frame is 64 ms
 * - compact rhythm telemetry at 10 Hz for smooth web/HUD animation
 */
#define APP_RHYTHM_TELEMETRY_MS 100
#define APP_RHYTHM_MAX_FRAME_DELTA_MS 220

static int64_t s_last_rhythm_telemetry_us = 0;
static uint32_t s_rhythm_tick_seq = 0;

/*
 * V10.3.2 hard guard:
 * Background review is the only feature allowed to seek backwards by more than
 * one note. It is armed ONLY by a real final-note completion on the ESP32.
 */
static bool s_review_seek_authorized = false;


/*
 * Violin Coach PC/Web/AR telemetry
 * --------------------------------
 * These helpers only mirror the existing application state to the serial port.
 * They do not participate in pitch detection, melody acceptance, voice intent,
 * display logic, or any other algorithm.
 *
 * Each machine-readable line is a standalone JSON object.  Human ESP-IDF logs
 * remain unchanged, so the original serial monitor workflow still works.
 */
static const char *vc_state_name(app_state_t state)
{
    switch (state) {
        case STATE_HOME: return "home";
        case STATE_SINGLE: return "single";
        case STATE_MELODY: return "melody";
        case STATE_PAUSED: return "paused";
        case STATE_VOICE: return "voice";
        case STATE_CLOUD: return "cloud";
        default: return "unknown";
    }
}

static const char *vc_practice_mode_name(practice_mode_t mode)
{
    switch (mode) {
        case PRACTICE_RHYTHM: return "rhythm";
        case PRACTICE_FULL: return "full";
        case PRACTICE_FOLLOW: return "follow";
        case PRACTICE_PITCH:
        default: return "pitch";
    }
}

static const char *vc_intent_name(intent_kind_t kind)
{
    switch (kind) {
        case INTENT_START_SINGLE: return "start_single";
        case INTENT_START_SONG: return "start_song";
        case INTENT_PAUSE: return "pause";
        case INTENT_CONTINUE: return "continue";
        case INTENT_NEXT: return "next";
        case INTENT_PREVIOUS: return "previous";
        case INTENT_TEMPO_UP: return "tempo_up";
        case INTENT_TEMPO_DOWN: return "tempo_down";
        case INTENT_TEMPO_RESET: return "tempo_reset";
        case INTENT_TEMPO_SET: return "tempo_set";
        case INTENT_MODE_PITCH: return "mode_pitch";
        case INTENT_MODE_RHYTHM: return "mode_rhythm";
        case INTENT_MODE_FULL: return "mode_full";
        case INTENT_MODE_FOLLOW: return "mode_follow";
        case INTENT_METRONOME_ON: return "metronome_on";
        case INTENT_METRONOME_OFF: return "metronome_off";
        case INTENT_REVIEW_START: return "review_start";
        case INTENT_REVIEW_SKIP: return "review_skip";
        case INTENT_END: return "end";
        case INTENT_PHRASE_REPEAT: return "phrase_repeat";
        case INTENT_PHRASE_SLOW_REPEAT: return "phrase_slow_repeat";
        case INTENT_PHRASE_NEXT: return "phrase_next";
        case INTENT_RESTART: return "restart";
        case INTENT_UNKNOWN:
        default: return "unknown";
    }
}

static void vc_json_write_string(const char *text)
{
    putchar('"');

    if (text) {
        const unsigned char *cursor =
            (const unsigned char *)text;

        while (*cursor) {
            unsigned char value = *cursor++;

            switch (value) {
                case '"':
                    fputs("\\\"", stdout);
                    break;
                case '\\':
                    fputs("\\\\", stdout);
                    break;
                case '\b':
                    fputs("\\b", stdout);
                    break;
                case '\f':
                    fputs("\\f", stdout);
                    break;
                case '\n':
                    fputs("\\n", stdout);
                    break;
                case '\r':
                    fputs("\\r", stdout);
                    break;
                case '\t':
                    fputs("\\t", stdout);
                    break;
                default:
                    if (value < 0x20) {
                        printf("\\u%04x", (unsigned)value);
                    } else {
                        putchar((int)value);
                    }
                    break;
            }
        }
    }

    putchar('"');
}


static float quarter_ms_locked(void)
{
    float tempo = s_song.tempo_bpm > 1.0f ? s_song.tempo_bpm : 90.0f;
    return 60000.0f / tempo;
}

static uint32_t rhythm_target_ms_locked(void)
{
    if (
        !s_song.events
        || s_song.count == 0
        || s_app.melody_index >= s_song.count
    ) {
        return 0;
    }
    return song_event_duration_ms(
        &s_song,
        &s_song.events[s_app.melody_index]
    );
}

static void rhythm_reset_accumulator_locked(void)
{
    s_app.rhythm_started = false;
    s_app.rhythm_onset_candidate_ms = 0;
    s_app.rhythm_boundary_candidate_ms = 0;
    s_app.rhythm_elapsed_ms = 0;
    s_app.rhythm_last_frame_us = 0;
    s_app.rhythm_elapsed_beats = 0.0f;
    s_app.rhythm_onset_ms = -1;
    s_app.rhythm_sounding_ms = 0;
    s_app.rhythm_correct_ms = 0;
    s_app.rhythm_cents_sum = 0.0f;
    s_app.rhythm_abs_cents_sum = 0.0f;
    s_app.rhythm_cents_sq_sum = 0.0f;
    s_app.rhythm_pitch_samples = 0;
}

static float song_beats_before_index_locked(size_t index)
{
    if (!s_song.events || s_song.count == 0) return 0.0f;
    if (index > s_song.count) index = s_song.count;

    float beats = 0.0f;
    for (size_t i = 0; i < index; ++i) {
        beats += s_song.events[i].beats > 0.0f
            ? s_song.events[i].beats
            : 1.0f;
    }
    return beats;
}

static void rhythm_sync_clock_to_index_locked(void)
{
    if(s_app.practice_mode==PRACTICE_FOLLOW && s_song.count){follow_begin_locked(true);return;}
    if (timed_mode() && s_song.count) {
        phrase_begin_locked(s_app.melody_index);
        return;
    }
    s_app.song_clock_beats =
        song_beats_before_index_locked(s_app.melody_index);
    rhythm_reset_accumulator_locked();
}

static void set_practice_mode_locked(practice_mode_t mode)
{
    if (s_app.practice_mode == mode) return; /* repeated ASR does not restart */
    s_app.practice_mode = mode;
    rhythm_sync_clock_to_index_locked();

    if (mode != PRACTICE_PITCH) {
        s_app.metronome_enabled = true;
        s_app.melody_release_required = false;
        s_app.melody_release_ms = 0;
        s_app.melody_stable_ms = 0;
    }

    ESP_LOGI(
        TAG,
        "practice mode = %s",
        vc_practice_mode_name(mode)
    );
}

static const char *rhythm_duration_status_locked(
    uint32_t target_ms,
    uint32_t actual_ms
)
{
    int32_t error_ms =
        (int32_t)actual_ms - (int32_t)target_ms;
    int32_t tolerance_ms =
        (int32_t)s_app.rhythm_tolerance_ms;

    if (error_ms < -tolerance_ms) return "short";
    if (error_ms > tolerance_ms) return "long";
    return "good";
}

static void vc_emit_rhythm_note_locked(
    size_t index,
    const song_event_t *event,
    uint32_t target_ms,
    uint32_t actual_ms,
    const char *boundary_reason
)
{
    if (!event) return;

    float mean_cents = 0.0f;
    float mean_abs_cents = 0.0f;
    float stability_cents = 0.0f;

    if (s_app.rhythm_pitch_samples > 0) {
        float n = (float)s_app.rhythm_pitch_samples;
        mean_cents = s_app.rhythm_cents_sum / n;
        mean_abs_cents = s_app.rhythm_abs_cents_sum / n;
        float mean_sq = s_app.rhythm_cents_sq_sum / n;
        float variance = mean_sq - mean_cents * mean_cents;
        if (variance < 0.0f) variance = 0.0f;
        stability_cents = sqrtf(variance);
    }

    int32_t duration_error_ms =
        (int32_t)actual_ms - (int32_t)target_ms;

    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"rhythm_note\",\"index\":%u,\"note\":",
        (unsigned)index
    );
    vc_json_write_string(event->note);
    printf(
        ",\"measure\":%u,\"beat\":%.3f,\"beats\":%.3f,"
        "\"target_ms\":%lu,\"actual_ms\":%lu,\"duration_error_ms\":%ld,"
        "\"duration_status\":",
        (unsigned)event->measure,
        event->beat,
        event->beats,
        (unsigned long)target_ms,
        (unsigned long)actual_ms,
        (long)duration_error_ms
    );
    vc_json_write_string(rhythm_duration_status_locked(target_ms, actual_ms));
    if (strcmp(boundary_reason ? boundary_reason : "", "score_clock") == 0) {
        const rt_note_t *n = &s_trainer.notes[index];
        const char *timing = n->rest ? (n->sounding_ms <= target_ms / 5 ? "good" : "rest_noise")
            : !n->hit ? "missed"
            : n->onset_ms < -(int)s_app.rhythm_tolerance_ms ? "early"
            : n->onset_ms > (int)s_app.rhythm_tolerance_ms ? "late" : "good";
        printf(",\"onset_detected\":%s,\"timing_status\":", n->hit ? "true" : "false");
        vc_json_write_string(timing);
    }
    printf(",\"phrase_run\":%lu",(unsigned long)s_phrase_run);
    printf(",\"boundary_reason\":");
    vc_json_write_string(
        boundary_reason ? boundary_reason : "unknown"
    );
    printf(
        ",\"onset_ms\":%ld,\"sounding_ms\":%lu,"
        "\"correct_ms\":%lu,\"mean_cents\":%.2f,\"mean_abs_cents\":%.2f,"
        "\"stability_cents\":%.2f,\"pitch_samples\":%lu,\"mode\":",
        (long)s_app.rhythm_onset_ms,
        (unsigned long)s_app.rhythm_sounding_ms,
        (unsigned long)s_app.rhythm_correct_ms,
        mean_cents,
        mean_abs_cents,
        stability_cents,
        (unsigned long)s_app.rhythm_pitch_samples
    );
    vc_json_write_string(
        vc_practice_mode_name(
            s_app.practice_mode
        )
    );
    printf("}\n");
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}


static void vc_emit_voice_phase(
    const char *phase,
    const char *source,
    const char *message
)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"voice\",\"phase\":");
    vc_json_write_string(phase ? phase : "unknown");
    printf(",\"source\":");
    vc_json_write_string(source ? source : "");
    printf(",\"message\":");
    vc_json_write_string(message ? message : "");
    printf(",\"wake_model\":");
    vc_json_write_string(wake_word_model_name());
    printf("}\n");
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}

static void vc_emit_control_ack(
    const char *command,
    bool ok,
    const char *value
)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"control_ack\",\"command\":");
    vc_json_write_string(command ? command : "");
    printf(",\"ok\":%s,\"value\":", ok ? "true" : "false");
    vc_json_write_string(value ? value : "");
    printf(",\"firmware\":");
    vc_json_write_string(VC_FIRMWARE_BUILD);
    printf("}\n");
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}

static float melody_acceptance_cents_locked(void)
{
    return !timed_mode()
        ? fmaxf(s_song.tolerance_cents, APP_MELODY_FLUID_MIN_CENTS)
        : s_song.tolerance_cents;
}

/* Include score identity/progress in both target changes and periodic state.
 * A browser or serial bridge may connect after the start event was emitted. */
static void vc_emit_score_fields_locked(void)
{
    const song_event_t *event = NULL;
    if (s_song.events && s_app.melody_index < s_song.count) {
        event = &s_song.events[s_app.melody_index];
    }
    bool active = s_app.state == STATE_MELODY
        || (s_app.resume_state == STATE_MELODY
            && (s_app.state == STATE_PAUSED || s_app.state == STATE_VOICE
                || s_app.state == STATE_CLOUD));
    printf(",\"song_id\":");
    vc_json_write_string(s_song.id);
    printf(",\"song_title\":");
    vc_json_write_string(s_song.title);
    printf(",\"song_active\":%s,\"song_index\":%u,\"song_count\":%u,"
           "\"tempo_bpm\":%.2f,\"tolerance_cents\":%.2f,\"practice_mode\":",
           active ? "true" : "false", (unsigned)s_app.melody_index,
           (unsigned)s_song.count, s_song.tempo_bpm,
           melody_acceptance_cents_locked());
    vc_json_write_string(vc_practice_mode_name(s_app.practice_mode));
    printf(",\"metronome_enabled\":%s,\"release_required\":%s,"
           "\"rhythm_tolerance_ms\":%lu,\"rhythm_started\":%s,"
           "\"rhythm_elapsed_ms\":%lu,\"rhythm_elapsed_beats\":%.6f,"
           "\"rhythm_target_ms\":%lu,\"song_clock_beats\":%.6f,"
           "\"finger\":%d,\"measure\":%u,\"beat\":%.3f",
           s_app.metronome_enabled ? "true" : "false",
           s_app.melody_release_required ? "true" : "false",
           (unsigned long)s_app.rhythm_tolerance_ms,
           s_app.rhythm_started ? "true" : "false",
           (unsigned long)s_app.rhythm_elapsed_ms, s_app.rhythm_elapsed_beats,
           (unsigned long)rhythm_target_ms_locked(), s_app.song_clock_beats,
           active && event ? event->finger : -1,
           active && event ? (unsigned)event->measure : 0,
           active && event ? event->beat : 0.0f);
    vc_emit_trainer_fields_locked();
}

static void vc_emit_target_locked(void)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf(
        "{\"vc\":\"target\",\"note\":"
    );

    vc_json_write_string(
        s_app.target_note
    );

    printf(
        ",\"hz\":%.3f,\"string_id\":%d,\"state\":",
        s_app.target_hz,
        s_app.target_string_id
    );
    vc_json_write_string(vc_state_name(s_app.state));
    vc_emit_score_fields_locked();
    printf("}\n");

    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}


static void vc_emit_state_locked(void)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf(
        "{\"vc\":\"state\",\"state\":"
    );

    vc_json_write_string(
        vc_state_name(s_app.state)
    );

    printf(
        ",\"resume_state\":"
    );

    vc_json_write_string(
        vc_state_name(s_app.resume_state)
    );

    printf(
        ",\"target\":"
    );

    vc_json_write_string(
        s_app.target_note
    );

    printf(
        ",\"mode\":"
    );

    vc_json_write_string(
        vc_practice_mode_name(
            s_app.practice_mode
        )
    );

    printf(",\"target_hz\":%.3f,\"string_id\":%d,\"firmware\":",
           s_app.target_hz, s_app.target_string_id);
    vc_json_write_string(VC_FIRMWARE_BUILD);
    vc_emit_score_fields_locked();
    printf("}\n");

    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}


static void vc_emit_song_action_locked(
    const char *action
)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf(
        "{\"vc\":\"song_action\",\"action\":"
    );

    vc_json_write_string(action);

    printf("}\n");

    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}


static void vc_emit_song_snapshot_locked(const char *action)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"song\",\"action\":");
    vc_json_write_string(action);
    printf(",\"id\":");
    vc_json_write_string(s_song.id);
    printf(",\"title\":");
    vc_json_write_string(s_song.title);
    printf(",\"count\":%u", (unsigned)s_song.count);
    vc_emit_score_fields_locked();
    printf("}\n");
    fflush(stdout);
    for (size_t index = 0; index < s_song.count; ++index) {
        const song_event_t *event = &s_song.events[index];
        printf("{\"vc\":\"song_note\",\"index\":%u,\"note\":", (unsigned)index);
        vc_json_write_string(event->note);
        printf(",\"beats\":%.3f,\"string_id\":%u,\"finger\":%d,"
               "\"measure\":%u,\"beat\":%.3f}\n", event->beats,
               (unsigned)event->string_id, event->finger,
               (unsigned)event->measure, event->beat);
        fflush(stdout);
    }
    xSemaphoreGive(s_telemetry_mutex);
}

static void vc_emit_song_definition_locked(void)
{
    vc_emit_song_snapshot_locked("start");
}


static void vc_emit_asr_text(
    const char *text
)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf(
        "{\"vc\":\"asr\",\"text\":"
    );

    vc_json_write_string(text);

    printf("}\n");

    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}

static void vc_emit_intent(intent_result_t intent)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"intent\",\"kind\":");
    vc_json_write_string(vc_intent_name(intent.kind));
    printf(",\"note\":");
    vc_json_write_string(intent.note);
    printf(",\"song\":");
    vc_json_write_string(intent.song_query);
    printf(",\"string_id\":%d", intent.string_id);
    printf(",\"tempo_bpm\":%.2f", intent.tempo_bpm);
    printf("}\n");
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}

static void app_lock(void)
{
    xSemaphoreTake(
        s_app_mutex,
        portMAX_DELAY
    );
}

static void app_unlock(void)
{
    xSemaphoreGive(s_app_mutex);
}

static void route_audio_for_state(
    app_state_t state
)
{
    if (
        state == STATE_SINGLE
        || state == STATE_MELODY
    ) {
        audio_inmp441_set_route(
            AUDIO_ROUTE_PITCH
        );
    } else {
        audio_inmp441_set_route(
            AUDIO_ROUTE_IDLE
        );
    }
}

static void start_single_locked(
    const char *note,
    int string_id
)
{
    if (
        !note
        || !note_is_supported(note)
    ) {
        return;
    }

    s_app.state = STATE_SINGLE;
    s_app.resume_state = STATE_SINGLE;
    s_app.practice_mode = PRACTICE_PITCH;
    rt_suspend(&s_trainer);
    s_follow_pitch_ok = false;
    s_follow_pitch_us = 0;

    strlcpy(
        s_app.target_note,
        note,
        sizeof(s_app.target_note)
    );

    s_app.target_hz =
        note_frequency(note);
    s_app.target_string_id =
        (string_id >= 1 && string_id <= 4) ? string_id : 0;

    s_app.melody_stable_ms = 0;
    s_app.melody_release_ms = 0;
    s_app.melody_release_required = false;
    s_app.song_clock_beats = 0.0f;
    rhythm_reset_accumulator_locked();

    vc_emit_target_locked();
}

static void set_song_target_locked(void)
{
    if (
        s_song.count == 0
        || !s_song.events
        || s_app.melody_index
            >= s_song.count
    ) {
        strlcpy(
            s_app.target_note,
            "REST",
            sizeof(s_app.target_note)
        );
        s_app.target_hz = 0.0f;
        s_app.target_string_id = 0;
        vc_emit_target_locked();
        return;
    }

    const song_event_t *event =
        &s_song.events[s_app.melody_index];

    strlcpy(
        s_app.target_note,
        event->note,
        sizeof(s_app.target_note)
    );

    s_app.target_string_id = event->string_id;

    if (
        strcmp(event->note, "REST")
        == 0
    ) {
        s_app.target_hz = 0.0f;
        s_app.target_string_id = 0;
    } else {
        s_app.target_hz =
            note_frequency(event->note);
    }

    vc_emit_target_locked();
}

static void start_song_locked(
    song_t *loaded
)
{
    if (
        !loaded
        || !loaded->events
        || loaded->count == 0
    ) {
        return;
    }

    song_library_free(&s_song);
    s_song = *loaded;
    memset(loaded, 0, sizeof(*loaded));
    s_song_original_tempo = s_song.tempo_bpm > 1.0f ? s_song.tempo_bpm : 90.0f;

    s_app.state = STATE_MELODY;
    s_app.resume_state = STATE_MELODY;
    s_app.melody_index = 0;
    s_app.melody_stable_ms = 0;
    s_app.melody_release_ms = 0;
    s_app.melody_release_required = false;
    s_review_seek_authorized = false;
    s_app.song_clock_beats = 0.0f;
    rhythm_reset_accumulator_locked();
    if(s_app.practice_mode==PRACTICE_FOLLOW)follow_begin_locked(true);
    else if(timed_mode())phrase_begin_locked(0);

    vc_emit_song_definition_locked();
    set_song_target_locked();
    vc_emit_state_locked();
}


static float song_measure_quarters_locked(void)
{
    unsigned n = s_song.time_num ? s_song.time_num : 4;
    unsigned d = s_song.time_den ? s_song.time_den : 4;
    return (float)n * 4.0f / (float)d;
}

static void follow_begin_locked(bool count_in)
{
    memset(&s_trainer,0,sizeof(s_trainer));
    rhythm_reset_accumulator_locked();
    s_follow_pitch_ok=false;s_follow_pitch_us=0;
    bool rest=s_song.events && s_app.melody_index<s_song.count &&
        !strcmp(s_song.events[s_app.melody_index].note,"REST");
    uint32_t prep=count_in?(uint32_t)(quarter_ms_locked()*song_measure_quarters_locked()):0;
    ft_begin(&s_follow,rhythm_target_ms_locked(),rest,prep,esp_timer_get_time());
    if(count_in){s_app.song_clock_beats=-song_measure_quarters_locked();++s_phrase_run;}
    s_review_seek_authorized=false;
}

static void phrase_begin_locked(size_t first)
{
    memset(&s_trainer, 0, sizeof(s_trainer));
    s_trainer.count = s_song.count > RT_MAX_NOTES ? RT_MAX_NOTES : s_song.count;
    float start = 0;
    for (size_t i = 0; i < s_trainer.count; ++i) {
        rt_note_t *n = &s_trainer.notes[i];
        n->start = start;
        n->beats = s_song.events[i].beats > 0 ? s_song.events[i].beats : 1;
        n->measure = s_song.events[i].measure ? s_song.events[i].measure
            : (uint16_t)(start / song_measure_quarters_locked()) + 1;
        n->rest = strcmp(s_song.events[i].note, "REST") == 0;
        start += n->beats;
    }
    rt_begin(&s_trainer, first, s_app.practice_mode == PRACTICE_FULL,
             song_measure_quarters_locked());
    s_trainer.last_us=esp_timer_get_time();
    s_trainer.offset_ms=s_rhythm_offset_ms;
    ++s_phrase_run;
    s_app.melody_index = first;
    s_app.song_clock_beats = s_trainer.clock;
    rhythm_reset_accumulator_locked();
    s_app.melody_release_required = false;
    s_review_seek_authorized = false;
}

static void vc_emit_trainer_fields_locked(void)
{
    if(s_app.practice_mode==PRACTICE_FOLLOW){
        printf(",\"phrase_run\":%lu,\"rhythm_offset_ms\":%d,\"control_generation\":%lu,\"clock_lag_ms\":0,"
               "\"metronome_clock_beats\":%.6f,\"time_num\":%u,\"time_den\":%u,"
               "\"follow_phase\":",(unsigned long)s_phrase_run,s_rhythm_offset_ms,(unsigned long)s_control_generation,s_app.song_clock_beats,
               (unsigned)(s_song.time_num?s_song.time_num:4),(unsigned)(s_song.time_den?s_song.time_den:4));
        vc_json_write_string(ft_phase_name(s_follow.phase));
        printf(",\"trainer_phase\":");vc_json_write_string(ft_phase_name(s_follow.phase));
        printf(",\"follow_elapsed_ms\":%lu,\"follow_target_ms\":%lu,"
               "\"follow_remaining_prep_ms\":%lu,\"follow_retries\":%lu,"
               "\"follow_pitch_ok\":%s,\"follow_sounding\":%s,\"follow_armed\":%s,\"follow_retry_reason\":%u,"
               "\"phrase_has_next\":false",
               (unsigned long)s_follow.elapsed_ms,(unsigned long)s_follow.target_ms,
               (unsigned long)s_follow.remaining_ms,(unsigned long)s_follow.retries,
               s_follow.pitch_ok?"true":"false",s_follow.sounding?"true":"false",s_follow.armed?"true":"false",s_follow.retry_reason);
        return;
    }
    unsigned good=0, early=0, late=0, missed=0, rests=0;
    float abs_error=0;
    unsigned hits=0;
    for (size_t i=s_trainer.first; i<s_trainer.end && i<s_trainer.count; ++i) {
        const rt_note_t *n=&s_trainer.notes[i];
        if (n->rest) { if (n->sounding_ms > n->beats * quarter_ms_locked() / 5) ++rests; continue; }
        if (!n->hit) { ++missed; continue; }
        ++hits; abs_error += fabsf((float)n->onset_ms);
        if (n->onset_ms < -(int)s_app.rhythm_tolerance_ms) ++early;
        else if (n->onset_ms > (int)s_app.rhythm_tolerance_ms) ++late;
        else ++good;
    }
    /* Continuous score clock: never reset at note boundaries (including pickups). */
    float metro=s_app.song_clock_beats;
    if(s_app.practice_mode!=PRACTICE_PITCH && s_trainer.count){
        metro=s_trainer.clock-s_trainer.notes[s_trainer.first].start
            +s_song.events[s_trainer.first].beat-1;
    }
    printf(",\"clock_lag_ms\":%.2f",s_trainer.last_us?(double)(esp_timer_get_time()-s_trainer.last_us)/1000.0:0.0);
    printf(",\"phrase_run\":%lu,\"rhythm_offset_ms\":%d",(unsigned long)s_phrase_run,s_rhythm_offset_ms);
    printf(",\"control_generation\":%lu,\"metronome_clock_beats\":%.6f",(unsigned long)s_control_generation,metro);
    printf(",\"time_num\":%u,\"time_den\":%u,\"trainer_phase\":",
           (unsigned)(s_song.time_num ? s_song.time_num : 4),
           (unsigned)(s_song.time_den ? s_song.time_den : 4));
    vc_json_write_string(s_app.practice_mode == PRACTICE_PITCH ? "idle" : rt_phase_name(s_trainer.phase));
    printf(",\"phrase_first\":%u,\"phrase_end\":%u,\"phrase_has_next\":%s,"
           "\"phrase_good\":%u,\"phrase_early\":%u,\"phrase_late\":%u,"
           "\"phrase_missed\":%u,\"phrase_rest_noise\":%u,\"phrase_mean_abs_ms\":%.1f",
           (unsigned)s_trainer.first, (unsigned)s_trainer.end,
           s_trainer.end < s_trainer.count ? "true" : "false",
           good, early, late, missed, rests, hits ? abs_error/hits : 0);
}

static void phrase_command_locked(intent_kind_t kind)
{
    if (!s_song.count || s_app.resume_state != STATE_MELODY ||
        !timed_mode()) return;
    size_t first = s_trainer.first;
    if (kind == INTENT_PHRASE_NEXT || kind == INTENT_NEXT) {
        if (s_trainer.end >= s_song.count) return; /* final phrase stays at end */
        first = s_trainer.end;
    } else if (kind == INTENT_PREVIOUS) {
        uint16_t m = s_song.events[first].measure;
        while (first > 0 && s_song.events[first-1].measure + 2 >= m) --first;
    } else if (kind == INTENT_PHRASE_SLOW_REPEAT) {
        s_song.tempo_bpm = fmaxf(40.0f, s_song.tempo_bpm * 0.85f);
    }
    phrase_begin_locked(first);
    s_app.state = s_app.resume_state = STATE_MELODY;
    set_song_target_locked();
}

/* A USB transfer is transactional. The live score is not freed until COMMIT
 * validates every indexed note and the checksum. ACKs permit bounded retries. */
static bool s_host_library=false;
static song_request_t s_usb_request;
static song_t s_usb_score;
static size_t s_usb_received=0;
static uint32_t s_usb_hash=2166136261u;
static int64_t s_usb_deadline=0;
static uint32_t s_usb_committed=0;
static uint32_t score_hash(uint32_t h,const char *s)
{ for(;*s;++s){h^=(unsigned char)*s;h*=16777619u;}return h; }
static bool decode_hex(const char *src,char *dst,size_t cap)
{
    size_t n=strlen(src);if(!n || n%2 || n/2>=cap)return false;
    for(size_t i=0;i<n;i+=2){unsigned v=0;char pair[3]={src[i],src[i+1],0};char *end;
        v=(unsigned)strtoul(pair,&end,16);if(*end || v==0)return false;dst[i/2]=(char)v;}
    dst[n/2]=0;return true;
}
static void score_ack(uint32_t id,const char *stage,int index,bool ok)
{
    xSemaphoreTake(s_telemetry_mutex,portMAX_DELAY);
    printf("{\"vc\":\"score_ack\",\"request_id\":%lu,\"stage\":\"%s\",\"index\":%d,\"ok\":%s}\n",
        (unsigned long)id,stage,index,ok?"true":"false");fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}
static void vc_emit_song_request_locked(const song_request_t *r, const char *phase, const char *message)
{
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"song_request\",\"request_id\":%lu,\"query\":", (unsigned long)r->generation);
    vc_json_write_string(r->query);
    printf(",\"transport\":\"%s\",\"phase\":",s_host_library?"usb":"http"); vc_json_write_string(phase);
    printf(",\"message\":"); vc_json_write_string(message);
    printf(",\"title\":"); vc_json_write_string(s_song.title);
    printf(",\"song_index\":%u}\n", (unsigned)s_app.melody_index);
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}

/* Queue length one: a newer selection supersedes an older waiting request.
 * The network worker never owns the app mutex while waiting for HTTP. */
static void request_song(const char *query, app_state_t previous, uint32_t expected)
{
    app_lock();
    if (expected != UINT32_MAX && expected != s_control_generation) {
        app_unlock(); return;
    }
    if (previous == STATE_PAUSED && s_loading_generation == s_control_generation && s_loading_generation != 0)
        previous = s_loading_restore_state;
    song_request_t r = {.generation=++s_control_generation, .restore_state=previous};
    s_loading_generation=0;song_library_free(&s_usb_score);
    strlcpy(r.query, query, sizeof(r.query));
    if (s_app.resume_state == STATE_MELODY && (previous == STATE_MELODY || previous == STATE_PAUSED)) {
        if (strcmp(query,s_song.title)==0 || strcmp(query,s_song.id)==0) {
            s_app.state = previous;
            rt_suspend(&s_trainer);
            vc_emit_song_request_locked(&r,"unchanged","同一首曲目，已保留当前进度；从头练请明确说重新开始");
            vc_emit_state_locked();
            app_unlock(); show_current_state(); return;
        }
    }
    s_review_seek_authorized = false;
    if (previous == STATE_MELODY || previous == STATE_SINGLE) s_app.resume_state = previous;
    s_app.state = STATE_PAUSED;
    s_loading_generation = r.generation;
    s_loading_restore_state = previous;
    rt_suspend(&s_trainer);
    vc_emit_song_request_locked(&r,"loading","正在加载新曲，当前进度已保留");
    vc_emit_state_locked();
    if(s_host_library){
        s_usb_request=r;s_usb_deadline=esp_timer_get_time()+10000000;
        song_library_free(&s_usb_score);s_usb_received=0;
    }else xQueueOverwrite(s_song_requests, &r);
    app_unlock();
    show_current_state();
}

static void song_loader_task(void *argument)
{
    (void)argument;
    song_request_t r;
    while (true) {
        if (xQueueReceive(s_song_requests, &r, portMAX_DELAY) != pdTRUE) continue;
        song_t loaded={0};
        esp_err_t error = song_library_fetch_strict(r.query, &loaded);
        app_lock();
        if (r.generation != s_control_generation) {
            vc_emit_song_request_locked(&r,"cancelled","较早的请求已取消，保留最新操作");
        } else if (error != ESP_OK || !loaded.events || loaded.count == 0) {
            s_app.state = r.restore_state;
            vc_emit_song_request_locked(&r,"failed","新曲加载失败，原曲与进度未替换；请检查曲库服务和网络");
            vc_emit_state_locked();
        } else if (s_song.count && strcmp(loaded.id,s_song.id)==0 &&
                   (r.restore_state==STATE_MELODY || r.restore_state==STATE_PAUSED) &&
                   s_app.resume_state==STATE_MELODY) {
            s_app.state = r.restore_state;
            vc_emit_song_request_locked(&r,"unchanged","同一首曲目，已保留当前进度");
            vc_emit_state_locked();
        } else {
            if (s_song.count) vc_emit_song_action_locked("stop");
            start_song_locked(&loaded);
            vc_emit_song_request_locked(&r,"applied","新曲已加载，乐谱与板子已切换");
        }
        if (s_loading_generation == r.generation) s_loading_generation = 0;
        app_unlock();
        song_library_free(&loaded);
        show_current_state();
    }
}

static void build_progress(
    char *output,
    size_t capacity,
    const app_context_t *snapshot,
    size_t song_count
)
{
    if (!output || capacity == 0) return;
    output[0] = '\0';

    if (
        snapshot
        && snapshot->state
            == STATE_MELODY
    ) {
        if (
            snapshot->melody_release_required
        ) {
            snprintf(
                output,
                capacity,
                "SONG %u/%u RELEASE",
                (unsigned)(
                    snapshot->melody_index + 1
                ),
                (unsigned)song_count
            );
        } else {
            snprintf(
                output,
                capacity,
                "SONG %u/%u",
                (unsigned)(
                    snapshot->melody_index + 1
                ),
                (unsigned)song_count
            );
        }
    }
}

static void show_current_state(void)
{
    app_lock();
    app_context_t snapshot = s_app;
    size_t song_count = s_song.count;
    app_unlock();

    if(snapshot.state==STATE_VOICE || snapshot.state==STATE_CLOUD)return;
    route_audio_for_state(
        snapshot.state
    );

    if (snapshot.state == STATE_HOME) {
        led_bar_off();
        display_show_home(
            wifi_time_is_connected()
        );
        return;
    }

    if (snapshot.state == STATE_PAUSED) {
        led_bar_off();
        display_show_message(
            "PAUSED",
            "SAY WAKE WORD",
            "THEN CONTINUE"
        );
        return;
    }

    if (
        snapshot.state == STATE_SINGLE
        || snapshot.state == STATE_MELODY
    ) {
        char progress[32];
        build_progress(
            progress,
            sizeof(progress),
            &snapshot,
            song_count
        );

        display_show_practice(
            snapshot.target_note,
            snapshot.target_hz,
            false,
            0.0f,
            0.0f,
            0.0f,
            progress
        );
    }
}

static float clamp_tempo(float bpm)
{
    if (bpm < 40.0f) return 40.0f;
    if (bpm > 220.0f) return 220.0f;
    return bpm;
}

static void set_song_tempo_locked(float bpm)
{
    if (s_song.count == 0) return;
    if(s_app.state==STATE_MELODY && timed_mode() && s_trainer.last_us){
        rt_tick(&s_trainer,esp_timer_get_time(),s_song.tempo_bpm);
        s_app.song_clock_beats=s_trainer.clock;
    }
    s_song.tempo_bpm = clamp_tempo(bpm);
    if(s_app.practice_mode==PRACTICE_FOLLOW && s_app.resume_state==STATE_MELODY)follow_begin_locked(true);
    ESP_LOGI(TAG, "song tempo = %.1f BPM", s_song.tempo_bpm);
}

static void seek_song_locked(size_t index)
{
    if (s_song.count == 0 || !s_song.events) return;
    if (index >= s_song.count) index = s_song.count - 1;
    s_app.melody_index = index;
    s_app.state = STATE_MELODY;
    s_app.resume_state = STATE_MELODY;
    s_app.melody_stable_ms = 0;
    s_app.melody_release_ms = 0;
    s_app.melody_release_required = false;
    rhythm_sync_clock_to_index_locked();
    set_song_target_locked();
}

static void apply_non_song_intent(
    intent_result_t intent, uint32_t expected
)
{
    app_lock();
    if (expected != UINT32_MAX && expected != s_control_generation) { app_unlock(); return; }
    bool setting=intent.kind==INTENT_TEMPO_UP || intent.kind==INTENT_TEMPO_DOWN ||
        intent.kind==INTENT_TEMPO_RESET || intent.kind==INTENT_TEMPO_SET ||
        intent.kind==INTENT_METRONOME_ON || intent.kind==INTENT_METRONOME_OFF;
    if(!setting){
        ++s_control_generation;
        if(s_loading_generation){
            s_app.state=s_loading_restore_state;
            if(s_host_library)vc_emit_song_request_locked(&s_usb_request,"cancelled","换曲已被新的控制操作取消，原进度保留");
            s_loading_generation=0;song_library_free(&s_usb_score);
        }
    }
    if (s_app.state == STATE_VOICE || s_app.state == STATE_CLOUD) s_app.state = s_app.resume_state;

    switch (intent.kind) {
        case INTENT_START_SINGLE:
            start_single_locked(intent.note, intent.string_id);
            break;

        case INTENT_PAUSE:
            if(s_app.practice_mode==PRACTICE_FOLLOW)follow_begin_locked(true);
            rt_suspend(&s_trainer);
            if (
                s_app.state == STATE_SINGLE
                || s_app.state
                    == STATE_MELODY
            ) {
                s_app.resume_state =
                    s_app.state;
            }
            s_app.state = STATE_PAUSED;
            break;

        case INTENT_CONTINUE:
            if(s_app.practice_mode==PRACTICE_FOLLOW && s_follow.phase==FT_DONE)break;
            if(s_app.practice_mode==PRACTICE_FOLLOW)follow_begin_locked(true);
            rt_suspend(&s_trainer);
            if (timed_mode() && s_trainer.phase == RT_FINISHED && s_app.resume_state == STATE_MELODY) {
                phrase_command_locked(INTENT_PHRASE_NEXT); break;
            }
            if (s_app.resume_state == STATE_HOME) break;
            if (
                s_app.resume_state
                == STATE_MELODY
                && s_song.count > 0
            ) {
                s_app.state =
                    STATE_MELODY;
            } else {
                s_app.state =
                    STATE_SINGLE;
            }
            break;

        case INTENT_NEXT:
            if (s_app.resume_state == STATE_HOME) break;
            if (timed_mode() && s_app.resume_state == STATE_MELODY) {
                phrase_command_locked(INTENT_NEXT); break;
            }
            if (
                (
                    s_app.state
                        == STATE_MELODY
                    || s_app.resume_state
                        == STATE_MELODY
                )
                && s_song.count > 0
            ) {
                if (
                    s_app.melody_index + 1
                    < s_song.count
                ) {
                    ++s_app.melody_index;
                }
                s_app.state = STATE_MELODY;
                s_app.resume_state =
                    STATE_MELODY;
                s_app.melody_stable_ms = 0;
                s_app.melody_release_ms = 0;
                s_app.melody_release_required =
                    false;
                rhythm_sync_clock_to_index_locked();
                set_song_target_locked();
            } else {
                char next_note[
                    NOTE_NAME_CAPACITY
                ];
                if (
                    note_step(
                        s_app.target_note,
                        1,
                        next_note,
                        sizeof(next_note)
                    )
                ) {
                    start_single_locked(
                        next_note,
                        s_app.target_string_id
                    );
                }
            }
            break;

        case INTENT_PREVIOUS:
            if (s_app.resume_state == STATE_HOME) break;
            if (timed_mode() && s_app.resume_state == STATE_MELODY) {
                phrase_command_locked(INTENT_PREVIOUS); break;
            }
            if (
                (
                    s_app.state
                        == STATE_MELODY
                    || s_app.resume_state
                        == STATE_MELODY
                )
                && s_song.count > 0
            ) {
                if (s_app.melody_index > 0) {
                    --s_app.melody_index;
                }
                s_app.state = STATE_MELODY;
                s_app.resume_state =
                    STATE_MELODY;
                s_app.melody_stable_ms = 0;
                s_app.melody_release_ms = 0;
                s_app.melody_release_required =
                    false;
                rhythm_sync_clock_to_index_locked();
                set_song_target_locked();
            } else {
                char previous_note[
                    NOTE_NAME_CAPACITY
                ];
                if (
                    note_step(
                        s_app.target_note,
                        -1,
                        previous_note,
                        sizeof(previous_note)
                    )
                ) {
                    start_single_locked(
                        previous_note,
                        s_app.target_string_id
                    );
                }
            }
            break;

        case INTENT_TEMPO_UP:
            if (s_song.count > 0) {
                set_song_tempo_locked(s_song.tempo_bpm * 1.10f);
            }
            break;

        case INTENT_TEMPO_DOWN:
            if (s_song.count > 0) {
                set_song_tempo_locked(s_song.tempo_bpm / 1.10f);
            }
            break;

        case INTENT_TEMPO_RESET:
            if (s_song.count > 0) {
                set_song_tempo_locked(s_song_original_tempo);
            }
            break;

        case INTENT_TEMPO_SET:
            if (s_song.count > 0 && intent.tempo_bpm > 0.0f) {
                set_song_tempo_locked(intent.tempo_bpm);
            }
            break;

        case INTENT_MODE_PITCH:
            set_practice_mode_locked(PRACTICE_PITCH);
            break;

        case INTENT_MODE_RHYTHM:
            set_practice_mode_locked(PRACTICE_RHYTHM);
            break;

        case INTENT_MODE_FOLLOW:
            set_practice_mode_locked(PRACTICE_FOLLOW);
            break;

        case INTENT_MODE_FULL:
            set_practice_mode_locked(PRACTICE_FULL);
            break;

        case INTENT_METRONOME_ON:
            s_app.metronome_enabled = true;
            break;

        case INTENT_METRONOME_OFF:
            s_app.metronome_enabled = false;
            break;

        case INTENT_REVIEW_START:
        case INTENT_REVIEW_SKIP:
            /* Orange Pi / PC teaching engine owns weak-section review. */
            break;

        case INTENT_PHRASE_REPEAT:
        case INTENT_PHRASE_SLOW_REPEAT:
        case INTENT_PHRASE_NEXT:
            phrase_command_locked(intent.kind); break;
        case INTENT_RESTART:
            if (s_song.count && s_app.resume_state == STATE_MELODY) {
                seek_song_locked(0);
            }
            break;
        case INTENT_END:
            rt_suspend(&s_trainer);
            s_trainer.phase = RT_IDLE;
            if (s_song.count > 0) {
                vc_emit_song_action_locked("stop");
            }
            s_review_seek_authorized = false;
            s_app.state = STATE_HOME;
            s_app.resume_state = STATE_HOME;
            break;

        case INTENT_START_SONG:
        case INTENT_UNKNOWN:
        default:
            break;
    }

    vc_emit_state_locked();

    app_state_t new_state = s_app.state;
    app_unlock();

    route_audio_for_state(new_state);
}



static void vc_maybe_emit_rhythm_tick_locked(int64_t now_us)
{
    if (
        now_us <= 0
        || (
            s_last_rhythm_telemetry_us > 0
            && now_us - s_last_rhythm_telemetry_us
                < (int64_t)APP_RHYTHM_TELEMETRY_MS * 1000LL
        )
    ) {
        return;
    }

    s_last_rhythm_telemetry_us = now_us;
    ++s_rhythm_tick_seq;

    uint32_t live_actual_ms =
        s_app.rhythm_elapsed_ms > s_app.rhythm_boundary_candidate_ms
            ? s_app.rhythm_elapsed_ms - s_app.rhythm_boundary_candidate_ms
            : 0;

    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf(
        "{\"vc\":\"rhythm_tick\",\"seq\":%lu,"
        "\"song_index\":%u,\"started\":%s,"
        "\"candidate_ms\":%lu,\"boundary_ms\":%lu,"
        "\"elapsed_ms\":%lu,\"actual_ms\":%lu,"
        "\"target_ms\":%lu,\"clock_beats\":%.6f,"
        "\"onset_ms\":%ld,\"tempo_bpm\":%.2f",
        (unsigned long)s_rhythm_tick_seq,
        (unsigned)s_app.melody_index,
        s_app.rhythm_started ? "true" : "false",
        (unsigned long)s_app.rhythm_onset_candidate_ms,
        (unsigned long)s_app.rhythm_boundary_candidate_ms,
        (unsigned long)s_app.rhythm_elapsed_ms,
        (unsigned long)live_actual_ms,
        (unsigned long)rhythm_target_ms_locked(),
        s_app.song_clock_beats,
        (long)s_app.rhythm_onset_ms,
        s_song.tempo_bpm
    );
    vc_emit_trainer_fields_locked();
    printf("}\n");
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);
}


static void phrase_emit_note_locked(size_t i)
{
    const rt_note_t *n=&s_trainer.notes[i];
    s_app.rhythm_onset_ms=n->hit ? n->onset_ms : -1;
    s_app.rhythm_sounding_ms=n->sounding_ms;
    s_app.rhythm_correct_ms=n->correct_ms;
    s_app.rhythm_pitch_samples=n->samples;
    s_app.rhythm_cents_sum=n->cents_sum;
    s_app.rhythm_abs_cents_sum=n->abs_cents_sum;
    s_app.rhythm_cents_sq_sum=n->cents_sq_sum;
    vc_emit_rhythm_note_locked(i,&s_song.events[i],
        song_event_duration_ms(&s_song,&s_song.events[i]),n->sounding_ms,"score_clock");
}

static bool follow_process_audio_locked(const audio_rhythm_frame_t *f)
{
    if(s_app.state!=STATE_MELODY || !s_song.events || s_app.melody_index>=s_song.count ||
       f->center_us<=s_follow.last_us)return false;
    int64_t delta=f->center_us-s_follow.last_us;
    /* Missing PCM is handled locally by ft_tick, never as a new song/count-in. */
    bool sounding=f->rms>fmaxf(.0015f,f->noise*2.5f);
    bool pitch_ok=s_follow_pitch_ok && s_follow_pitch_index==s_app.melody_index &&
        s_follow_pitch_us>0 && llabs(f->center_us-s_follow_pitch_us)<=180000;
    if(delta<=250000)s_app.song_clock_beats+=(float)delta/1000/quarter_ms_locked();
    bool next=ft_tick(&s_follow,f->center_us,sounding,pitch_ok);
    s_app.rhythm_started=s_follow.phase==FT_HOLDING;
    s_app.rhythm_elapsed_ms=s_follow.elapsed_ms;
    s_app.rhythm_elapsed_beats=(float)s_follow.elapsed_ms/quarter_ms_locked();
    if(next){
        if(s_app.melody_index+1<s_song.count){
            ++s_app.melody_index;follow_begin_locked(false);set_song_target_locked();
        }else{
            s_app.state=STATE_PAUSED;s_app.resume_state=STATE_MELODY;
            s_app.rhythm_started=false;
            vc_emit_song_action_locked("done");
        }
        vc_emit_state_locked();
    }
    vc_maybe_emit_rhythm_tick_locked(f->center_us);
    return next;
}

static bool rhythm_process_audio_locked(const audio_rhythm_frame_t *f)
{
    if(s_app.practice_mode==PRACTICE_FOLLOW)return follow_process_audio_locked(f);
    if(s_app.state!=STATE_MELODY || s_app.practice_mode==PRACTICE_PITCH || !s_song.events ||
       s_trainer.phase==RT_IDLE || s_trainer.phase==RT_FINISHED || f->center_us<=s_trainer.last_us)return false;
    int64_t previous_us=s_trainer.last_us;
    size_t old=s_trainer.cursor;
    bool finished=rt_tick(&s_trainer,f->center_us,s_song.tempo_bpm);
    if(f->onset)rt_onset(&s_trainer,f->center_us,s_song.tempo_bpm,s_app.rhythm_tolerance_ms);
    bool sounding=f->rms>fmaxf(.0015f,f->noise*2.5f);
    if(s_trainer.phase==RT_PLAYING && previous_us>0){
        rt_note_t *n=&s_trainer.notes[s_trainer.cursor];
        float remaining=fmaxf(0,n->start+n->beats-fmaxf(n->start,
            s_trainer.clock-(float)(f->center_us-previous_us)*s_song.tempo_bpm/60000000));
        unsigned dt=(unsigned)fminf((float)(f->center_us-previous_us)/1000,
            remaining*quarter_ms_locked());
        if(sounding)n->sounding_ms+=dt;
    }
    /* Late attacks may arrive after a short note ends. Finalize after the
       matching window rather than emitting a premature 'missed'. */
    while(s_trainer.emitted<s_trainer.end && (finished ||
        (s_trainer.clock-s_trainer.notes[s_trainer.emitted].start)*quarter_ms_locked()>300)){
        if(!finished && s_trainer.emitted>=s_trainer.cursor)break;
        phrase_emit_note_locked(s_trainer.emitted++);
    }
    s_app.song_clock_beats=fminf(s_trainer.clock,s_trainer.end_beat);
    s_app.melody_index=s_trainer.cursor;
    if(finished){
        s_app.melody_index=s_trainer.end-1;
        s_app.state=STATE_PAUSED;s_app.resume_state=STATE_MELODY;
        s_app.rhythm_started=false;s_review_seek_authorized=false;
        vc_emit_state_locked();return true;
    }
    s_app.rhythm_started=s_trainer.phase==RT_PLAYING;
    s_app.rhythm_elapsed_beats=fmaxf(0,s_trainer.clock-s_trainer.notes[s_trainer.cursor].start);
    s_app.rhythm_elapsed_ms=(uint32_t)(s_app.rhythm_elapsed_beats*quarter_ms_locked());
    s_app.rhythm_onset_ms=s_trainer.notes[s_trainer.cursor].hit?s_trainer.notes[s_trainer.cursor].onset_ms:-1;
    if(s_trainer.cursor!=old){set_song_target_locked();vc_emit_state_locked();}
    vc_maybe_emit_rhythm_tick_locked(f->center_us);
    return false;
}

static void rhythm_task(void *arg)
{
    (void)arg;
    audio_rhythm_frame_t f;
    while(true){
        app_lock();
        if(s_host_library && s_loading_generation && s_loading_generation==s_control_generation && s_loading_generation==s_usb_request.generation &&
           esp_timer_get_time()>s_usb_deadline){
            s_app.state=s_usb_request.restore_state;s_loading_generation=0;song_library_free(&s_usb_score);
            vc_emit_song_request_locked(&s_usb_request,"failed","USB曲库响应超时，保留原曲；请确认Orange Pi新版服务正在运行");
            vc_emit_state_locked();app_unlock();show_current_state();
        }else app_unlock();
        if(audio_inmp441_get_rhythm_frame(&f,pdMS_TO_TICKS(100))!=ESP_OK)continue;
        app_lock();bool finished=rhythm_process_audio_locked(&f);app_unlock();
        if(finished)show_current_state();
    }
}

static void pitch_mode_clock_tick_locked(void)
{
    if (
        s_app.state != STATE_MELODY
        || s_app.practice_mode != PRACTICE_PITCH
    ) {
        return;
    }

    s_app.song_clock_beats +=
        (float)APP_RHYTHM_FRAME_MS
        / quarter_ms_locked();
}

static float median3(
    float a,
    float b,
    float c
)
{
    if (a > b) {
        float temp = a;
        a = b;
        b = temp;
    }
    if (b > c) {
        float temp = b;
        b = c;
        c = temp;
    }
    if (a > b) {
        float temp = a;
        a = b;
        b = temp;
    }
    return b;
}

static float frame_rms(
    const float *frame
)
{
    if (!frame) return 0.0f;

    double sum_square = 0.0;

    for (
        int index = 0;
        index < APP_FFT_SIZE;
        ++index
    ) {
        sum_square +=
            frame[index] * frame[index];
    }

    return sqrtf(
        (float)(
            sum_square / APP_FFT_SIZE
        )
    );
}

static void pitch_task(void *argument)
{
    (void)argument;

    float *frame = heap_caps_malloc(
        APP_FFT_SIZE * sizeof(float),
        MALLOC_CAP_SPIRAM
            | MALLOC_CAP_8BIT
    );

    if (!frame) {
        ESP_LOGE(
            TAG,
            "pitch frame allocation failed"
        );
        vTaskDelete(NULL);
        return;
    }

    float history[3] = {0};
    int history_count = 0;
    int history_index = 0;
    int invalid_streak = 0;
    float held_frequency = 0.0f;
    float held_cents = 0.0f;
    float held_confidence = 0.0f;
    float previous_result_rms = 0.0f;
    char history_target[NOTE_NAME_CAPACITY] = "";
    TickType_t last_display = 0;

    while (true) {
        if (
            audio_inmp441_get_pitch_frame(
                frame,
                pdMS_TO_TICKS(500)
            )
            != ESP_OK
        ) {
            continue;
        }

        app_lock();
        app_context_t snapshot = s_app;
        uint32_t pitch_generation=s_control_generation,pitch_run=s_phrase_run;

        size_t song_count = s_song.count;
        song_event_t melody_event = {0};
        float melody_tolerance =
            APP_MELODY_ACCEPT_CENTS;
        uint32_t required_ms =
            APP_MELODY_HOLD_MS;

        if (
            snapshot.state == STATE_MELODY
            && s_song.events
            && snapshot.melody_index
                < s_song.count
        ) {
            melody_event =
                s_song.events[
                    snapshot.melody_index
                ];
            melody_tolerance =
                melody_acceptance_cents_locked();
            required_ms =
                song_event_required_ms(
                    &s_song,
                    &melody_event
                );
        }
        app_unlock();

        if (
            snapshot.state != STATE_SINGLE
            && snapshot.state
                != STATE_MELODY
        ) {
            history_count = 0;
            invalid_streak = 0;
            held_frequency = 0.0f;
            held_cents = 0.0f;
            held_confidence = 0.0f;
            previous_result_rms = 0.0f;
            history_target[0] = '\0';
            continue;
        }

        if (
            snapshot.state == STATE_MELODY
            && snapshot.practice_mode == PRACTICE_PITCH
        ) {
            app_lock();
            if (
                s_app.state == STATE_MELODY
                && s_app.melody_index == snapshot.melody_index
            ) {
                pitch_mode_clock_tick_locked();
            }
            snapshot = s_app;
            app_unlock();
        }

        if (
            strcmp(
                history_target,
                snapshot.target_note
            ) != 0
        ) {
            history_count = 0;
            history_index = 0;
            invalid_streak = 0;
            held_frequency = 0.0f;
            held_cents = 0.0f;
            held_confidence = 0.0f;
            strlcpy(
                history_target,
                snapshot.target_note,
                sizeof(history_target)
            );
        }

        bool melody_finished = false;

        /*
         * REST事件不进入HPS：要求保持静音到对应节拍时长。
         */
        if (
            snapshot.state == STATE_MELODY
            && strcmp(
                melody_event.note,
                "REST"
            ) == 0
        ) {
            float rms = frame_rms(frame);
            float gate = fmaxf(
                APP_HPS_MIN_RMS,
                audio_inmp441_noise_rms()
                    * APP_HPS_NOISE_MULT
            );

            if (snapshot.practice_mode != PRACTICE_PITCH) {
                led_bar_off();
                app_lock();snapshot=s_app;song_count=s_song.count;app_unlock();
                TickType_t now = xTaskGetTickCount();
                if (
                    now - last_display
                    >= pdMS_TO_TICKS(180)
                ) {
                    char progress[32];
                    build_progress(
                        progress,
                        sizeof(progress),
                        &snapshot,
                        song_count
                    );
                    display_show_practice(
                        snapshot.target_note,
                        snapshot.target_hz,
                        false,
                        0.0f,
                        0.0f,
                        0.0f,
                        progress
                    );
                    last_display = now;
                }
                continue;
            }

            led_bar_off();

            app_lock();

            if (
                s_app.state == STATE_MELODY
                && s_app.melody_index
                    == snapshot.melody_index
            ) {
                if (rms < gate * 1.15f) {
                    s_app.melody_stable_ms
                        += 64;
                } else {
                    s_app.melody_stable_ms = 0;
                }

                if (
                    s_app.melody_stable_ms
                    >= required_ms
                ) {
                    s_app.melody_stable_ms = 0;

                    if (
                        s_app.melody_index + 1
                        < s_song.count
                    ) {
                        ++s_app.melody_index;
                        set_song_target_locked();
                    } else {
                        s_review_seek_authorized = true;
                        vc_emit_song_action_locked("done");
                        s_app.state = STATE_HOME;
                        s_app.resume_state = STATE_HOME;
                        vc_emit_state_locked();
                        melody_finished = true;
                    }
                }

                snapshot = s_app;
                song_count = s_song.count;
            }

            app_unlock();

            if (melody_finished) {
                route_audio_for_state(
                    STATE_HOME
                );
                display_show_message(
                    "SONG DONE",
                    "GOOD JOB",
                    "SAY WAKE WORD"
                );
                vTaskDelay(
                    pdMS_TO_TICKS(1200)
                );
                show_current_state();
                history_count = 0;
                continue;
            }

            TickType_t now =
                xTaskGetTickCount();

            if (
                now - last_display
                >= pdMS_TO_TICKS(300)
            ) {
                char progress[32];
                build_progress(
                    progress,
                    sizeof(progress),
                    &snapshot,
                    song_count
                );

                display_show_practice(
                    "REST",
                    0.0f,
                    false,
                    0.0f,
                    0.0f,
                    0.0f,
                    progress
                );

                last_display = now;
            }

            continue;
        }

        hps_result_t result;

        if (
            hps_pitch_analyze(
                frame,
                snapshot.target_hz,
                audio_inmp441_noise_rms(),
                &result
            )
            != ESP_OK
        ) {
            continue;
        }

        bool stable_valid = false;
        float stable_frequency =
            result.frequency_hz;
        float stable_cents =
            result.cents;
        float display_confidence =
            result.confidence;

        if (result.valid) {
            invalid_streak = 0;

            history[history_index] =
                result.frequency_hz;
            history_index =
                (history_index + 1) % 3;

            if (history_count < 3) {
                ++history_count;
            }

            if (history_count >= 3) {
                stable_frequency = median3(
                    history[0],
                    history[1],
                    history[2]
                );
                stable_cents = pitch_cents(
                    stable_frequency,
                    snapshot.target_hz
                );
                stable_valid = true;

                held_frequency =
                    stable_frequency;
                held_cents =
                    stable_cents;
                held_confidence =
                    result.confidence;
            }
        } else {
            ++invalid_streak;

            /*
             * V3.4显示保持：
             * 单个坏帧/擦弦瞬态不立刻让屏幕和LED全部消失。
             */
            if (
                history_count >= 3
                && invalid_streak
                    <= APP_PITCH_INVALID_GRACE_FRAMES
                && held_frequency > 0.0f
            ) {
                stable_valid = true;
                stable_frequency =
                    held_frequency;
                stable_cents =
                    held_cents;
                display_confidence =
                    held_confidence;
            } else if (
                invalid_streak
                    > APP_PITCH_INVALID_GRACE_FRAMES
            ) {
                history_count = 0;
                history_index = 0;
                held_frequency = 0.0f;
                held_cents = 0.0f;
                held_confidence = 0.0f;
            }
        }

        /*
         * Melody acceptance starts from the first HPS/YIN-valid frame.
         * Display smoothing still uses the original 3-frame median path above.
         * Use the current valid result for progress; held display values must
         * not accept silence or keep accumulating the previous pitch.
         */
        app_lock();
        if(s_app.practice_mode==PRACTICE_FOLLOW && s_app.state==STATE_MELODY &&
           s_app.melody_index==snapshot.melody_index && pitch_generation==s_control_generation &&
           pitch_run==s_phrase_run && !strcmp(s_app.target_note,snapshot.target_note)){
            s_follow_pitch_ok=result.valid && result.confidence>=.20f &&
                fabsf(result.cents)<=melody_acceptance_cents_locked();
            s_follow_pitch_us=esp_timer_get_time();s_follow_pitch_index=s_app.melody_index;
        }
        if(s_app.state==STATE_MELODY && timed_mode() &&
           s_app.melody_index==snapshot.melody_index && s_trainer.phase==RT_PLAYING &&
           result.valid && result.confidence>=.20f){
            rt_note_t *n=&s_trainer.notes[s_app.melody_index];
            n->samples++;n->cents_sum+=result.cents;
            n->abs_cents_sum+=fabsf(result.cents);n->cents_sq_sum+=result.cents*result.cents;
        }
        app_unlock();
        bool melody_accept_valid = result.valid;
        float melody_accept_cents =
            result.cents;

        if(snapshot.practice_mode!=PRACTICE_PITCH){
            size_t old_index=snapshot.melody_index;
            app_lock();snapshot=s_app;song_count=s_song.count;app_unlock();
            if(snapshot.melody_index!=old_index){
                history_count=history_index=invalid_streak=0;
                held_frequency=held_cents=held_confidence=0;stable_valid=false;
                strlcpy(history_target,snapshot.target_note,sizeof(history_target));
            }
        }
        app_lock();bool still_practicing=s_app.state==STATE_MELODY || s_app.state==STATE_SINGLE;app_unlock();
        if(!still_practicing){led_bar_off();continue;}

        led_bar_show_cents(
            stable_cents,
            stable_valid
        );

        if (
            snapshot.state == STATE_MELODY
            && snapshot.practice_mode == PRACTICE_PITCH
        ) {
            app_lock();

            if (
                s_app.state == STATE_MELODY
                && s_app.melody_index
                    == snapshot.melody_index
            ) {
                if (
                    s_app.melody_release_required
                ) {
                    /*
                     * 连续同音（如C4,C4）必须先检测到真实停弓/能量下降。
                     * 不能再依赖stable_valid=false，因为V3.4会保持显示，
                     * 会让用户已经停弓但release条件仍迟迟不成立。
                     */
                    bool strong_rebow_drop =
                        previous_result_rms
                            > result.noise_gate
                                * APP_MELODY_REBOW_MIN_GATE_MULT
                        && result.rms
                            < previous_result_rms
                                * APP_MELODY_REBOW_DROP_RATIO;

                    bool release_signal =
                        result.rms
                            < result.noise_gate
                                * APP_MELODY_RELEASE_GATE_MULT
                        || !result.valid
                        || strong_rebow_drop
                        || (
                            result.valid
                            && fabsf(result.cents)
                                > melody_tolerance + 80.0f
                        );

                    if (strong_rebow_drop) {
                        /*
                         * A clear bow re-articulation can be shorter than two
                         * 64 ms pitch frames. Treat a strong energy dip as a
                         * confirmed release so repeated notes do not feel stuck.
                         */
                        s_app.melody_release_ms =
                            APP_MELODY_RELEASE_CONFIRM_MS;
                    } else if (release_signal) {
                        s_app.melody_release_ms += 64;
                    } else if (s_app.melody_release_ms > 32) {
                        /* A single residual frame no longer erases all release progress. */
                        s_app.melody_release_ms -= 32;
                    } else {
                        s_app.melody_release_ms = 0;
                    }

                    if (
                        s_app.melody_release_ms
                        >= APP_MELODY_RELEASE_CONFIRM_MS
                    ) {
                        s_app.melody_release_required =
                            false;
                        s_app.melody_release_ms = 0;

                        history_count = 0;
                        history_index = 0;
                        invalid_streak = 0;
                        held_frequency = 0.0f;
                        held_cents = 0.0f;
                        held_confidence = 0.0f;

                        ESP_LOGI(
                            TAG,
                            "MELODY release accepted: %u/%u target=%s",
                            (unsigned)(
                                s_app.melody_index + 1
                            ),
                            (unsigned)s_song.count,
                            s_app.target_note
                        );
                    }
                } else if (
                    melody_accept_valid
                    && fabsf(melody_accept_cents)
                        <= melody_tolerance
                ) {
                    s_app.melody_stable_ms += 64;

                    uint32_t pass_ms =
                        required_ms;

                    /*
                     * V10.3 FLUID：
                     * HPS/YIN第一帧已经判定有效且进入目标容差后立即累计。
                     * ±25 cent 只需一个64ms分析步；较宽容差最多约128ms。
                     * 显示端仍保留3帧中值稳定，不影响逐音推进。
                     */
                    if (
                        fabsf(melody_accept_cents)
                            <= APP_CORRECT_CENTS
                    ) {
                        if (pass_ms > APP_MELODY_IN_TUNE_FAST_MS) {
                            pass_ms = APP_MELODY_IN_TUNE_FAST_MS;
                        }
                    } else if (
                        fabsf(melody_accept_cents)
                            <= APP_MELODY_NEAR_FAST_CENTS
                    ) {
                        if (pass_ms > APP_MELODY_NEAR_FAST_MS) {
                            pass_ms = APP_MELODY_NEAR_FAST_MS;
                        }
                    } else if (pass_ms > APP_MELODY_ACCEPT_MAX_MS) {
                        /* Still inside the song tolerance: do not make the learner wait ~0.6s. */
                        pass_ms = APP_MELODY_ACCEPT_MAX_MS;
                    }

                    if (
                        s_app.melody_stable_ms
                        >= pass_ms
                    ) {
                        char old_note[
                            NOTE_NAME_CAPACITY
                        ];
                        strlcpy(
                            old_note,
                            s_app.target_note,
                            sizeof(old_note)
                        );

                        ESP_LOGI(
                            TAG,
                            "MELODY accepted: %u/%u note=%s cents=%.1f hold=%lums need=%lums",
                            (unsigned)(
                                s_app.melody_index + 1
                            ),
                            (unsigned)s_song.count,
                            old_note,
                            melody_accept_cents,
                            (unsigned long)
                                s_app.melody_stable_ms,
                            (unsigned long)pass_ms
                        );

                        s_app.melody_stable_ms = 0;

                        if (
                            s_app.melody_index + 1
                            < s_song.count
                        ) {
                            ++s_app.melody_index;
                            set_song_target_locked();

                            s_app.melody_release_required =
                                strcmp(
                                    old_note,
                                    s_app.target_note
                                ) == 0;

                            /*
                             * 任何一次进入下一事件，都彻底清除上一音历史。
                             */
                            history_count = 0;
                            history_index = 0;
                            invalid_streak = 0;
                            held_frequency = 0.0f;
                            held_cents = 0.0f;
                            held_confidence = 0.0f;
                            stable_valid = false;
                            led_bar_off();

                            if (
                                s_app.melody_release_required
                            ) {
                                ESP_LOGI(
                                    TAG,
                                    "MELODY next note is repeated %s: waiting for release/re-bow",
                                    s_app.target_note
                                );
                            }
                        } else {
                            s_review_seek_authorized = true;
                            vc_emit_song_action_locked("done");
                            s_app.state = STATE_HOME;
                            s_app.resume_state = STATE_HOME;
                            vc_emit_state_locked();
                            melody_finished = true;
                            led_bar_off();
                        }
                    }
                } else {
                    /*
                     * 轻微的边缘帧不把累计进度瞬间清零；
                     * 明显错音才完全清零。
                     */
                    if (
                        melody_accept_valid
                        && fabsf(melody_accept_cents)
                            <= melody_tolerance
                                + 20.0f
                    ) {
                        if (
                            s_app.melody_stable_ms
                            > APP_MELODY_PROGRESS_DECAY_MS
                        ) {
                            s_app.melody_stable_ms
                                -= APP_MELODY_PROGRESS_DECAY_MS;
                        } else {
                            s_app.melody_stable_ms = 0;
                        }
                    } else {
                        s_app.melody_stable_ms = 0;
                    }
                }
            }

            snapshot = s_app;
            song_count = s_song.count;
            app_unlock();
        }

        if (melody_finished) {
            route_audio_for_state(
                STATE_HOME
            );
            display_show_message(
                "SONG DONE",
                "GOOD JOB",
                "SAY WAKE WORD"
            );
            vTaskDelay(
                pdMS_TO_TICKS(1200)
            );
            show_current_state();
            history_count = 0;
            continue;
        }

        previous_result_rms = result.rms;

        TickType_t now =
            xTaskGetTickCount();

        if (
            now - last_display
            >= pdMS_TO_TICKS(300)
        ) {
            char progress[32];
            build_progress(
                progress,
                sizeof(progress),
                &snapshot,
                song_count
            );

            if (
                snapshot.state == STATE_SINGLE
                || snapshot.state
                    == STATE_MELODY
            ) {
                display_show_practice(
                    snapshot.target_note,
                    snapshot.target_hz,
                    stable_valid,
                    stable_frequency,
                    stable_cents,
                    display_confidence,
                    progress
                );
            }

            last_display = now;
        }
    }
}

static void network_warmup_task(
    void *argument
)
{
    (void)argument;

    if (
        wifi_time_wait_connected(
            pdMS_TO_TICKS(30000)
        )
        == ESP_OK
    ) {
        esp_err_t error =
            wifi_time_sync_clock(
                pdMS_TO_TICKS(45000)
            );

        if (error == ESP_OK) {
            ESP_LOGI(
                TAG,
                "network warm-up complete"
            );
        } else {
            ESP_LOGW(
                TAG,
                "network warm-up time sync failed: %s",
                esp_err_to_name(error)
            );
        }
    } else {
        ESP_LOGW(
            TAG,
            "network warm-up: Wi-Fi not ready"
        );
    }

    vTaskDelete(NULL);
}

static void restore_previous_state(
    app_state_t previous, uint32_t generation
)
{
    app_lock();
    if (generation != s_control_generation) { app_unlock(); wake_word_set_enabled(true); return; }
    s_app.state = previous;
    rt_suspend(&s_trainer);
    vc_emit_state_locked();
    app_unlock();
    show_current_state();
    wake_word_set_enabled(true);
}

static void voice_task(void *argument)
{
    (void)argument;

    const size_t voice_samples =
        APP_MIC_SAMPLE_RATE
        * APP_VOICE_MAX_SECONDS;

    int16_t *voice_pcm =
        heap_caps_malloc(
            voice_samples
                * sizeof(int16_t),
            MALLOC_CAP_SPIRAM
                | MALLOC_CAP_8BIT
        );

    char recognized[256];

    if (!voice_pcm) {
        ESP_LOGE(
            TAG,
            "voice buffer allocation failed"
        );
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (
            wake_word_wait(portMAX_DELAY)
            != ESP_OK
        ) {
            continue;
        }

        vc_emit_voice_phase(
            "wake_detected",
            "wakenet_or_manual",
            "wake accepted; preparing command capture"
        );

        app_lock();
        app_state_t previous = s_app.state;
        uint32_t voice_generation = s_control_generation;
        if (previous == STATE_MELODY || previous == STATE_SINGLE) s_app.resume_state = previous;
        rt_suspend(&s_trainer);
        s_app.state = STATE_VOICE;
        vc_emit_state_locked();
        app_unlock();

        led_bar_off();
        display_show_message(
            "VOICE",
            "SPEAK NOW",
            "AUTO STOP"
        );
        vc_emit_voice_phase(
            "listening",
            "inmp441",
            "speak command now"
        );

        size_t written = 0;
        audio_voice_capture_info_t capture_info =
            {0};

        esp_err_t error =
            audio_inmp441_capture_voice(
                voice_pcm,
                voice_samples,
                &written,
                &capture_info,
                pdMS_TO_TICKS(
                    (
                        APP_VOICE_MAX_SECONDS
                        + 2
                    ) * 1000
                )
            );

        if (error == ESP_ERR_NOT_FOUND) {
            vc_emit_voice_phase(
                "no_speech",
                "inmp441",
                "no command speech detected"
            );
            display_show_message(
                "NO SPEECH",
                "SAY COMMAND",
                "AFTER VOICE"
            );
            vTaskDelay(
                pdMS_TO_TICKS(900)
            );
            restore_previous_state(previous, voice_generation);
            continue;
        }

        if (error != ESP_OK) {
            vc_emit_voice_phase(
                "mic_error",
                "inmp441",
                esp_err_to_name(error)
            );
            display_show_message(
                "MIC ERROR",
                esp_err_to_name(error),
                "CHECK INMP441"
            );
            vTaskDelay(
                pdMS_TO_TICKS(1200)
            );
            restore_previous_state(previous, voice_generation);
            continue;
        }

        ESP_LOGI(
            TAG,
            "command audio: %lu ms rms=%.5f gain=%.2f threshold=%.5f",
            (unsigned long)
                capture_info.duration_ms,
            capture_info.raw_rms,
            capture_info.gain_applied,
            capture_info.vad_threshold
        );

        app_lock();
        if (voice_generation != s_control_generation) { app_unlock(); wake_word_set_enabled(true); continue; }
        s_app.state = STATE_CLOUD;
        vc_emit_state_locked();
        app_unlock();

        display_show_message(
            "CLOUD ASR",
            "RECOGNIZING",
            "PLEASE WAIT"
        );
        vc_emit_voice_phase(
            "recognizing",
            "xfyun",
            "uploading command audio"
        );

        esp_err_t wifi_error =
            wifi_time_wait_connected(
                pdMS_TO_TICKS(30000)
            );

        if (wifi_error != ESP_OK) {
            vc_emit_voice_phase(
                "wifi_error",
                "wifi",
                "no IP address"
            );
            display_show_message(
                "WIFI ERROR",
                "NO IP ADDRESS",
                "SEE SERIAL LOG"
            );
            vTaskDelay(
                pdMS_TO_TICKS(1800)
            );
            restore_previous_state(previous, voice_generation);
            continue;
        }

        esp_err_t time_error =
            wifi_time_sync_clock(
                pdMS_TO_TICKS(45000)
            );

        if (time_error != ESP_OK) {
            vc_emit_voice_phase(
                "time_error",
                "ntp",
                "clock sync failed"
            );
            display_show_message(
                "TIME ERROR",
                "NTP FAILED",
                "SEE SERIAL LOG"
            );
            vTaskDelay(
                pdMS_TO_TICKS(1800)
            );
            restore_previous_state(previous, voice_generation);
            continue;
        }

        error = xfyun_asr_transcribe(
            voice_pcm,
            written,
            recognized,
            sizeof(recognized)
        );

        if (error != ESP_OK) {
            vc_emit_voice_phase(
                "asr_error",
                "xfyun",
                esp_err_to_name(error)
            );
            display_show_message(
                "ASR ERROR",
                "CHECK XFYUN",
                "SEE SERIAL LOG"
            );
            vTaskDelay(
                pdMS_TO_TICKS(1400)
            );
            restore_previous_state(previous, voice_generation);
            continue;
        }

        ESP_LOGI(
            TAG,
            "XFYUN text: %s",
            recognized
        );

        vc_emit_asr_text(recognized);
        vc_emit_voice_phase(
            "recognized",
            "xfyun",
            recognized
        );

        app_lock();
        bool stale_voice = voice_generation != s_control_generation;
        app_unlock();
        if (stale_voice) {
            vc_emit_voice_phase("cancelled","xfyun","识别期间有更新的操作，本次命令已取消");
            wake_word_set_enabled(true); continue;
        }

        intent_result_t intent =
            intent_parse(recognized);

        vc_emit_intent(intent);

        ESP_LOGI(
            TAG,
            "intent: kind=%d note=%s song=%s",
            (int)intent.kind,
            intent.note[0]
                ? intent.note
                : "-",
            intent.song_query[0]
                ? intent.song_query
                : "-"
        );

        if (
            intent.kind == INTENT_UNKNOWN
        ) {
            vc_emit_voice_phase("unknown_command","intent","未识别成操作，请说换成加完整曲名");
            display_show_message(
                "UNKNOWN CMD",
                "TRY AGAIN",
                "SEE SERIAL TEXT"
            );
            vTaskDelay(
                pdMS_TO_TICKS(1200)
            );
            restore_previous_state(previous, voice_generation);
            continue;
        }

        if (
            intent.kind == INTENT_NEXT
            || intent.kind == INTENT_PREVIOUS
        ) {
            TickType_t nav_now = xTaskGetTickCount();
            TickType_t nav_window = pdMS_TO_TICKS(APP_VOICE_NAV_DEBOUNCE_MS);

            if (
                s_last_voice_nav_kind == intent.kind
                && s_last_voice_nav_tick != 0
                && (nav_now - s_last_voice_nav_tick) < nav_window
            ) {
                ESP_LOGW(
                    TAG,
                    "VOICE nav duplicate ignored: %s",
                    vc_intent_name(intent.kind)
                );
                display_show_message(
                    "VOICE CMD",
                    "DUPLICATE IGNORED",
                    intent.kind == INTENT_NEXT ? "NEXT" : "PREVIOUS"
                );
                vTaskDelay(pdMS_TO_TICKS(250));
                restore_previous_state(previous, voice_generation);
                continue;
            }

            s_last_voice_nav_kind = intent.kind;
            s_last_voice_nav_tick = nav_now;
        }

        if (
            intent.kind == INTENT_REVIEW_START
            || intent.kind == INTENT_REVIEW_SKIP
        ) {
            /*
             * Review selection is decided by the Orange Pi/PC teacher engine,
             * because it owns the per-note history.  We emit the intent above,
             * then return to the exact state that existed before voice capture.
             */
            display_show_message(
                "TEACHER CMD",
                intent.kind == INTENT_REVIEW_START ? "START REVIEW" : "SKIP REVIEW",
                "SENT TO HOST"
            );
            vTaskDelay(pdMS_TO_TICKS(450));
            restore_previous_state(previous, voice_generation);
            continue;
        }

        if (intent.kind == INTENT_START_SONG) {
            request_song(intent.song_query, previous, voice_generation);
        } else {
            apply_non_song_intent(intent, voice_generation);
        }

        show_current_state();
        wake_word_set_enabled(true);
        vTaskDelay(
            pdMS_TO_TICKS(250)
        );
    }
}


/*
 * PC/Web/Orange Pi -> ESP32 serial control.
 *
 * Fixed commands:
 *   VC_CMD:NEXT / PREVIOUS / PAUSE / CONTINUE / END
 *   VC_CMD:TEMPO_UP / TEMPO_DOWN / TEMPO_RESET
 *   VC_CMD:VOICE  (manual voice test: bypass wake phrase once)
 *   VC_CMD:METRONOME:ON / METRONOME:OFF
 * Value commands:
 *   VC_CMD:TEMPO:72.0
 *   VC_CMD:MODE:PITCH / MODE:RHYTHM / MODE:FULL
 *   VC_CMD:RHYTHM_TOLERANCE:140
 *   VC_CMD:START_SONG:小星星
 *   VC_CMD:REVIEW_BEGIN:12 / REVIEW_SEEK:12 / REVIEW_END
 *   VC_CMD:SEEK:* is blocked in V10.3.2 to prevent accidental backward jumps.
 *
 * HPS/YIN pitch detection is not modified by this control channel.
 */
static bool score_transfer_command(const char *cmd)
{
    if(!strncmp(cmd,"HOST_LIBRARY:",13)){
        app_lock();s_host_library=!strcmp(cmd+13,"ON");app_unlock();
        vc_emit_control_ack("HOST_LIBRARY",true,s_host_library?"USB":"HTTP");return true;
    }
    if(strncmp(cmd,"SCORE_",6))return false;
    char stage[16]={0};unsigned id=0;
    if(sscanf(cmd,"SCORE_%15[^:]:%u",stage,&id)!=2)return true;
    app_lock();
    bool current=id==s_loading_generation && id==s_control_generation && id==s_usb_request.generation;
    bool ok=false;int index=-1;
    if(!strcmp(stage,"BEGIN") && current){
        unsigned count=0,hold=0,num=0,den=0;float bpm=0,tol=0;int used=0;
        if(sscanf(cmd,"SCORE_BEGIN:%u:%u:%f:%f:%u:%u:%u%n",&id,&count,&bpm,&tol,&hold,&num,&den,&used)==7 &&
           cmd[used]==0 && count>0 && count<=APP_MAX_SONG_NOTES && isfinite(bpm) && bpm>=20 && bpm<=300 &&
           isfinite(tol) && tol>=1 && tol<=100 && num>0 && num<=12 && (den==2||den==4||den==8||den==16)){
            song_library_free(&s_usb_score);s_usb_received=0;s_usb_hash=score_hash(2166136261u,cmd);
            s_usb_score.events=calloc(count,sizeof(song_event_t));
            if(s_usb_score.events){s_usb_score.count=count;s_usb_score.tempo_bpm=bpm;
                s_usb_score.tolerance_cents=tol;s_usb_score.hold_ms=hold;s_usb_score.time_num=num;s_usb_score.time_den=den;ok=true;}
        }
    }else if(current && s_usb_score.events && (!strcmp(stage,"TITLE")||!strcmp(stage,"ID"))){
        const char *p=strchr(cmd,':');p=p?strchr(p+1,':'):NULL;
        if(p){
            char decoded[72]={0};bool title=!strcmp(stage,"TITLE");
            char *dest=title?s_usb_score.title:s_usb_score.id;
            if(decode_hex(p+1,decoded,title?sizeof(s_usb_score.title):sizeof(s_usb_score.id))){
                if(dest[0])ok=!strcmp(dest,decoded);
                else if(title || s_usb_score.title[0]){strlcpy(dest,decoded,title?sizeof(s_usb_score.title):sizeof(s_usb_score.id));s_usb_hash=score_hash(s_usb_hash,cmd);ok=true;}
            }
        }
    }else if(current && s_usb_score.events && s_usb_score.title[0] && s_usb_score.id[0] && !strcmp(stage,"NOTE")){
        song_event_t e={0};unsigned i=0,measure=0;int sid=0,finger=-1,used=0;
        if(sscanf(cmd,"SCORE_NOTE:%u:%u:%7[^:]:%f:%d:%d:%u:%f%n",&id,&i,e.note,&e.beats,&sid,&finger,&measure,&e.beat,&used)==8 &&
           cmd[used]==0 && i<s_usb_score.count && (note_is_supported(e.note)||!strcmp(e.note,"REST")) &&
           isfinite(e.beats) && e.beats>0 && e.beats<=64 && sid>=0 && sid<=4 && finger>=-1 && finger<=4 &&
           measure>0 && measure<=65535 && isfinite(e.beat) && e.beat>=1 && e.beat<=64){
            index=(int)i;e.string_id=sid;e.finger=finger;e.measure=measure;
            if(i<s_usb_received){
                const song_event_t *old=&s_usb_score.events[i];
                ok=!strcmp(old->note,e.note) && old->beats==e.beats && old->string_id==e.string_id &&
                    old->finger==e.finger && old->measure==e.measure && old->beat==e.beat;
            }
            else if(i==s_usb_received){s_usb_score.events[i]=e;++s_usb_received;s_usb_hash=score_hash(s_usb_hash,cmd);ok=true;}
        }
    }else if(current && !strcmp(stage,"ABORT")){
        s_app.state=s_usb_request.restore_state;s_loading_generation=0;
        song_library_free(&s_usb_score);
        vc_emit_song_request_locked(&s_usb_request,"failed","曲库传输失败，原曲和进度保留；查看网页具体错误");
        vc_emit_state_locked();ok=true;
    }else if(current && !strcmp(stage,"COMMIT")){
        unsigned hash=0;int used=0;
        if(sscanf(cmd,"SCORE_COMMIT:%u:%x%n",&id,&hash,&used)==2 && cmd[used]==0 &&
           s_usb_score.events && s_usb_received==s_usb_score.count && hash==s_usb_hash &&
           s_usb_score.title[0] && s_usb_score.id[0]){
            if(s_song.count && !strcmp(s_song.id,s_usb_score.id) && s_app.resume_state==STATE_MELODY &&
               (s_usb_request.restore_state==STATE_MELODY || s_usb_request.restore_state==STATE_PAUSED)){
                s_app.state=s_usb_request.restore_state;
                vc_emit_song_request_locked(&s_usb_request,"unchanged","同一曲目，保留进度；从头练请点重新开始");
            }else{
                if(s_song.count)vc_emit_song_action_locked("stop");
                start_song_locked(&s_usb_score);
                vc_emit_song_request_locked(&s_usb_request,"applied","USB乐谱校验通过，板子与网页已切换");
            }
            song_library_free(&s_usb_score);s_loading_generation=0;s_usb_committed=id;
            vc_emit_state_locked();ok=true;
        }
    }else if(!strcmp(stage,"COMMIT") && id==s_usb_request.generation && !s_loading_generation){
        /* Lost commit ACK: retry acknowledges the already applied transaction. */
        ok=id==s_usb_committed;
    }
    if(current)s_usb_deadline=esp_timer_get_time()+10000000;
    app_unlock();score_ack(id,stage,index,ok);
    if(ok && (!strcmp(stage,"COMMIT")||!strcmp(stage,"ABORT")))show_current_state();
    return true;
}

static void serial_control_task(void *argument)
{
    (void)argument;

    char line[192];
    size_t used=0;bool overflow=false;
    setvbuf(stdin, NULL, _IONBF, 0);
    while(true){
        int ch=getchar();
        if(ch==EOF){clearerr(stdin);vTaskDelay(pdMS_TO_TICKS(2));continue;}
        if(ch=='\r')continue;
        if(ch!='\n'){
            if(used<sizeof(line)-1)line[used++]=(char)ch;
            else overflow=true;
            continue;
        }
        line[used]=0;used=0;
        if(overflow){overflow=false;vc_emit_control_ack("LINE",false,"too_long");continue;}

        size_t length = strlen(line);
        while (
            length > 0
            && (
                line[length - 1] == '\r'
                || line[length - 1] == '\n'
                || line[length - 1] == ' '
                || line[length - 1] == '\t'
            )
        ) {
            line[--length] = '\0';
        }

        if (strncmp(line, "VC_CMD:", 7) != 0) {
            continue;
        }

        const char *command = line + 7;
        ESP_LOGI(TAG, "HOST control: %s", command);
        if(score_transfer_command(command))continue;
        if(!strncmp(command,"RHYTHM_OFFSET:",14)){
            char *end;long ms=strtol(command+14,&end,10);
            bool ok=*end==0 && end!=command+14 && ms>=-300 && ms<=300;
            if(ok){app_lock();s_rhythm_offset_ms=(int)ms;s_trainer.offset_ms=(int)ms;vc_emit_state_locked();app_unlock();}
            vc_emit_control_ack("RHYTHM_OFFSET",ok,command+14);continue;
        }

        if (strncmp(command, "START_NOTE:", 11) == 0) {
            char note[NOTE_NAME_CAPACITY];
            bool ok=note_parse(command+11,note,sizeof(note)) && note_is_supported(note);
            if(ok){intent_result_t intent={.kind=INTENT_START_SINGLE};
                strlcpy(intent.note,note,sizeof(intent.note));
                apply_non_song_intent(intent,UINT32_MAX);
            }
            vc_emit_control_ack("START_NOTE",ok,ok?note:command+11);
            continue;
        }

        if (strcmp(command, "SYNC") == 0) {
            app_lock();
            if (s_song.events && s_song.count > 0) {
                vc_emit_song_snapshot_locked("sync");
            }
            vc_emit_state_locked();
            app_unlock();
            continue;
        }

        if (strcmp(command, "VOICE") == 0) {
            vc_emit_voice_phase(
                "manual_trigger",
                "web",
                "manual voice capture requested"
            );
            esp_err_t wake_error = wake_word_trigger();
            if (wake_error != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "HOST voice trigger rejected: %s",
                    esp_err_to_name(wake_error)
                );
                vc_emit_voice_phase(
                    "busy",
                    "web",
                    "voice pipeline is already busy"
                );
            }
            continue;
        }

        if (
            strncmp(command, "MODE:", 5) == 0
            || strcmp(command, "PITCH") == 0
            || strcmp(command, "RHYTHM") == 0
            || strcmp(command, "FULL") == 0
        ) {
            const char *mode =
                strncmp(command, "MODE:", 5) == 0
                ? command + 5
                : command;
            bool ok = true;

            app_lock();
            if (strcmp(mode, "PITCH") == 0) {
                set_practice_mode_locked(PRACTICE_PITCH);
            } else if (strcmp(mode, "RHYTHM") == 0) {
                set_practice_mode_locked(PRACTICE_RHYTHM);
            } else if (strcmp(mode, "FOLLOW") == 0) {
                set_practice_mode_locked(PRACTICE_FOLLOW);
            } else if (strcmp(mode, "FULL") == 0) {
                set_practice_mode_locked(PRACTICE_FULL);
            } else {
                ok = false;
            }

            const char *applied =
                ok ? vc_practice_mode_name(s_app.practice_mode) : mode;

            if (ok) {
                vc_emit_state_locked();
            }
            app_unlock();

            vc_emit_control_ack("MODE", ok, applied);

            if (ok) {
                show_current_state();
            } else {
                ESP_LOGW(TAG, "unknown practice mode: %s", mode);
            }
            continue;
        }

        if (strncmp(command, "METRONOME:", 10) == 0) {
            const char *value = command + 10;
            app_lock();
            if (strcmp(value, "ON") == 0) {
                s_app.metronome_enabled = true;
            } else if (strcmp(value, "OFF") == 0) {
                s_app.metronome_enabled = false;
            }
            vc_emit_state_locked();
            app_unlock();
            continue;
        }

        if (strncmp(command, "RHYTHM_TOLERANCE:", 17) == 0) {
            char *endptr = NULL;
            long requested = strtol(command + 17, &endptr, 10);
            if (endptr != command + 17) {
                if (requested < APP_RHYTHM_MIN_TOLERANCE_MS) {
                    requested = APP_RHYTHM_MIN_TOLERANCE_MS;
                }
                if (requested > APP_RHYTHM_MAX_TOLERANCE_MS) {
                    requested = APP_RHYTHM_MAX_TOLERANCE_MS;
                }
                app_lock();
                s_app.rhythm_tolerance_ms = (uint32_t)requested;
                vc_emit_state_locked();
                app_unlock();
            }
            continue;
        }

        if (strncmp(command, "START_SONG:", 11) == 0) {
            if (!command[11]) continue;
            app_lock();
            app_state_t previous = (s_app.state == STATE_VOICE || s_app.state == STATE_CLOUD)
                ? s_app.resume_state : s_app.state;
            app_unlock();
            request_song(command+11, previous, UINT32_MAX);
            continue;
        }

        /*
         * V10.3.2:
         * Plain SEEK is intentionally disabled. It was too dangerous because a
         * stale Teacher Engine event could move a live song back toward index 0.
         *
         * REVIEW_BEGIN / REVIEW_SEEK are accepted only after the ESP32 itself
         * completed the final score event and armed s_review_seek_authorized.
         */
        if (strncmp(command, "SEEK:", 5) == 0) {
            ESP_LOGW(TAG, "HOST SEEK blocked by V10.3.2 anti-jump guard");
            continue;
        }

        if (strncmp(command, "REVIEW_BEGIN:", 13) == 0) {
            char *endptr = NULL;
            long requested = strtol(command + 13, &endptr, 10);
            if (endptr != command + 13 && requested >= 0) {
                app_lock();
                bool allowed = s_review_seek_authorized;
                if (allowed) {
                    seek_song_locked((size_t)requested);
                    vc_emit_state_locked();
                }
                app_unlock();

                if (allowed) {
                    ESP_LOGI(TAG, "review begin at index=%ld", requested);
                    show_current_state();
                } else {
                    ESP_LOGW(
                        TAG,
                        "REVIEW_BEGIN rejected: song has not completed normally"
                    );
                }
            }
            continue;
        }

        if (strncmp(command, "REVIEW_SEEK:", 12) == 0) {
            char *endptr = NULL;
            long requested = strtol(command + 12, &endptr, 10);
            if (endptr != command + 12 && requested >= 0) {
                app_lock();
                bool allowed = s_review_seek_authorized;
                if (allowed) {
                    seek_song_locked((size_t)requested);
                    vc_emit_state_locked();
                }
                app_unlock();

                if (allowed) {
                    ESP_LOGI(TAG, "review loop seek index=%ld", requested);
                    show_current_state();
                } else {
                    ESP_LOGW(TAG, "REVIEW_SEEK rejected: not authorized");
                }
            }
            continue;
        }

        if (strcmp(command, "REVIEW_END") == 0) {
            app_lock();
            s_review_seek_authorized = false;
            app_unlock();
            ESP_LOGI(TAG, "review seek authorization cleared");
            continue;
        }

        if (strncmp(command, "TEMPO:", 6) == 0) {
            char *endptr = NULL;
            float bpm = strtof(command + 6, &endptr);
            if (endptr != command + 6 && bpm > 0.0f) {
                float applied_bpm = 0.0f;
                app_lock();
                set_song_tempo_locked(bpm);
                applied_bpm = s_song.tempo_bpm;
                vc_emit_state_locked();
                app_unlock();

                char tempo_value[24];
                snprintf(
                    tempo_value,
                    sizeof(tempo_value),
                    "%.1f",
                    applied_bpm
                );
                vc_emit_control_ack(
                    "TEMPO",
                    true,
                    tempo_value
                );
                show_current_state();
            } else {
                vc_emit_control_ack(
                    "TEMPO",
                    false,
                    command + 6
                );
            }
            continue;
        }

        intent_result_t intent = {0};
        if (strcmp(command, "NEXT") == 0) {
            intent.kind = INTENT_NEXT;
        } else if (strcmp(command, "PREVIOUS") == 0) {
            intent.kind = INTENT_PREVIOUS;
        } else if (strcmp(command, "PAUSE") == 0) {
            intent.kind = INTENT_PAUSE;
        } else if (strcmp(command, "CONTINUE") == 0) {
            intent.kind = INTENT_CONTINUE;
        } else if (strcmp(command, "END") == 0) {
            intent.kind = INTENT_END;
        } else if (strcmp(command, "PHRASE_REPEAT") == 0) {
            intent.kind = INTENT_PHRASE_REPEAT;
        } else if (strcmp(command, "PHRASE_SLOW_REPEAT") == 0) {
            intent.kind = INTENT_PHRASE_SLOW_REPEAT;
        } else if (strcmp(command, "PHRASE_NEXT") == 0) {
            intent.kind = INTENT_PHRASE_NEXT;
        } else if (strcmp(command, "RESTART") == 0) {
            intent.kind = INTENT_RESTART;
        } else if (strcmp(command, "TEMPO_UP") == 0) {
            intent.kind = INTENT_TEMPO_UP;
        } else if (strcmp(command, "TEMPO_DOWN") == 0) {
            intent.kind = INTENT_TEMPO_DOWN;
        } else if (strcmp(command, "TEMPO_RESET") == 0) {
            intent.kind = INTENT_TEMPO_RESET;
        } else {
            continue;
        }

        apply_non_song_intent(intent, UINT32_MAX);
        app_lock();
        vc_emit_state_locked();
        app_unlock();
        show_current_state();
    }
}

static void diagnostic_task(void *argument)
{
    (void)argument;

    while (true) {
#if APP_MIC_DIAGNOSTIC
        app_lock();
        app_context_t snapshot = s_app;
        app_unlock();

        ESP_LOGI(
            "MIC_DIAG",
            "RMS=%.6f noise=%.6f peak=%ld state=%d target=%s",
            audio_inmp441_last_rms(),
            audio_inmp441_noise_rms(),
            (long)audio_inmp441_last_peak(),
            (int)snapshot.state,
            snapshot.target_note
        );
#endif
        /* Keep browser/PC synchronized even when it connects mid-session. */
        app_lock();
        vc_emit_state_locked();
        app_unlock();

        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );
    }
}

void app_main(void)
{
    s_app_mutex = xSemaphoreCreateMutex();
    s_telemetry_mutex = xSemaphoreCreateMutex();
    s_song_requests = xQueueCreate(1, sizeof(song_request_t));
    configASSERT(s_app_mutex && s_telemetry_mutex && s_song_requests);
    s_app.practice_mode = PRACTICE_PITCH;
    s_app.metronome_enabled = false;
    s_app.rhythm_tolerance_ms = APP_RHYTHM_DEFAULT_TOLERANCE_MS;
    s_app.rhythm_onset_ms = -1;

    ESP_ERROR_CHECK(
        display_st7789_init()
    );
    ESP_ERROR_CHECK(
        led_bar_init()
    );
    ESP_ERROR_CHECK(
        hps_pitch_init()
    );
    /*
     * ESP-SR wake word model is stored in a separate model partition.
     * Do not reboot the whole firmware when the model partition is empty
     * or the model has not been flashed yet. LCD/HPS can continue working.
     */
    esp_err_t wake_ret = wake_word_init();
    if (wake_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "wake word disabled: model partition not available (%s)",
            esp_err_to_name(wake_ret)
        );
    }
    ESP_ERROR_CHECK(
        audio_inmp441_init()
    );

    /*
     * Wi-Fi失败不会影响本地HPS。
     * 语音云端和网页曲库在需要时再检查网络。
     */
    wifi_time_init();

    app_lock();

#if APP_BOOT_STARTS_A4
    start_single_locked("A4", 2);
#else
    s_app.state = STATE_HOME;
    s_app.resume_state = STATE_HOME;
    strlcpy(
        s_app.target_note,
        "A4",
        sizeof(s_app.target_note)
    );
    s_app.target_hz =
        note_frequency("A4");
#endif

    vc_emit_state_locked();
    app_unlock();

    show_current_state();

    xTaskCreatePinnedToCore(
        pitch_task,
        "hps_pitch",
        8192,
        NULL,
        8,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        voice_task,
        "voice_cloud",
        15360,
        NULL,
        7,
        NULL,
        1
    );

    xTaskCreate(
        network_warmup_task,
        "network_warmup",
        4096,
        NULL,
        4,
        NULL
    );

    xTaskCreate(song_loader_task, "song_loader", 8192, NULL, 4, NULL);
    /* Keep PCM consumption independent of the heavier pitch analysis on core 1. */
    xTaskCreatePinnedToCore(rhythm_task,"rhythm_pcm",4096,NULL,9,NULL,0);

    xTaskCreate(
        serial_control_task,
        "web_serial_ctrl",
        3072,
        NULL,
        3,
        NULL
    );

    xTaskCreate(
        diagnostic_task,
        "mic_diag",
        3072,
        NULL,
        2,
        NULL
    );

    ESP_LOGI(
        TAG,
        "V3 ready; note range C3-C8"
    );
    ESP_LOGI(
        TAG,
        "buttonless wake word: 你好小智; model=%s",
        wake_word_model_name()
    );
    xSemaphoreTake(s_telemetry_mutex, portMAX_DELAY);
    printf("{\"vc\":\"hello\",\"firmware\":");
    vc_json_write_string(VC_FIRMWARE_BUILD);
    printf(",\"mode_control\":true,\"rhythm_engine\":true}\n");
    fflush(stdout);
    xSemaphoreGive(s_telemetry_mutex);

    vc_emit_voice_phase(
        "ready",
        "wakenet",
        "wake phrase: 你好小智"
    );
    ESP_LOGI(
        TAG,
        "song library base URL: %s",
        APP_SONG_LIBRARY_BASE_URL
    );
}
