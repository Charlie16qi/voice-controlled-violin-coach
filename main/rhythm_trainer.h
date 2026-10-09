#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RT_MAX_NOTES 256
typedef enum { RT_IDLE, RT_COUNT_IN, RT_PLAYING, RT_FINISHED } rt_phase_t;
typedef struct {
    float start, beats;
    uint16_t measure;
    bool rest, hit;
    int32_t onset_ms;
    uint32_t sounding_ms, correct_ms, samples;
    float cents_sum, abs_cents_sum, cents_sq_sum;
} rt_note_t;
typedef struct {
    rt_phase_t phase;
    size_t count, first, end, cursor;
    rt_note_t notes[RT_MAX_NOTES];
    float clock, end_beat;
    int offset_ms;
    size_t emitted;
    int64_t last_us, candidate_us, last_onset_us;
    float last_hz, candidate_hz, previous_rms;
    unsigned candidate_frames, quiet_frames;
    bool armed, baseline_needed;
} rhythm_trainer_t;

/* All durations and the clock use quarter-note units, including 6/8 music. */
void rt_begin(rhythm_trainer_t *t, size_t first, bool full, float count_in);
void rt_suspend(rhythm_trainer_t *t);
bool rt_tick(rhythm_trainer_t *t, int64_t now_us, float bpm);
void rt_audio(rhythm_trainer_t *t, int64_t now_us, float bpm,
              bool sound, bool valid, float hz, float confidence,
              float rms, float gate, unsigned tolerance_ms);
const char *rt_phase_name(rt_phase_t phase);

void rt_onset(rhythm_trainer_t *t, int64_t onset_us, float bpm, unsigned tolerance_ms);
