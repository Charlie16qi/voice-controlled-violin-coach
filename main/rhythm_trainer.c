#include "rhythm_trainer.h"
#include <math.h>

const char *rt_phase_name(rt_phase_t p)
{
    switch (p) {
        case RT_COUNT_IN: return "count_in";
        case RT_PLAYING: return "playing";
        case RT_FINISHED: return "phrase_end";
        default: return "idle";
    }
}

void rt_suspend(rhythm_trainer_t *t)
{
    t->last_us = 0;
    t->candidate_frames = 0;
    t->baseline_needed = true;
}

void rt_begin(rhythm_trainer_t *t, size_t first, bool full, float count_in)
{
    if (!t->count || first >= t->count) { t->phase = RT_IDLE; return; }
    t->first = t->cursor = t->emitted = first;
    t->end = first + 1;
    while (t->end < t->count && (full ||
           t->notes[t->end].measure < t->notes[first].measure + 2)) ++t->end;
    t->end_beat = t->notes[t->end - 1].start + t->notes[t->end - 1].beats;
    t->clock = t->notes[first].start - count_in;
    t->phase = count_in > 0 ? RT_COUNT_IN : RT_PLAYING;
    t->last_us = t->candidate_us = t->last_onset_us = 0;
    t->last_hz = t->candidate_hz = t->previous_rms = 0;
    t->candidate_frames = t->quiet_frames = 0;
    t->armed = true;
    t->baseline_needed = false;
    for (size_t i = first; i < t->end; ++i) {
        rt_note_t *n = &t->notes[i];
        n->hit = false; n->onset_ms = 0;
        n->sounding_ms = n->correct_ms = n->samples = 0;
        n->cents_sum = n->abs_cents_sum = n->cents_sq_sum = 0;
    }
}

bool rt_tick(rhythm_trainer_t *t, int64_t now_us, float bpm)
{
    if (t->phase != RT_COUNT_IN && t->phase != RT_PLAYING) return false;
    if (!t->last_us) { t->last_us = now_us; return false; }
    int64_t delta = now_us - t->last_us;
    t->last_us = now_us;
    if (delta <= 0 || bpm <= 0) return false;
    /* Do not cap elapsed time: a delayed frame must not slow the beat clock. */
    t->clock += (float)delta * bpm / 60000000.0f;
    if (t->clock >= t->notes[t->first].start) t->phase = RT_PLAYING;
    while (t->cursor + 1 < t->end &&
           t->clock >= t->notes[t->cursor + 1].start) ++t->cursor;
    if (t->clock + 0.00001f >= t->end_beat + 0.30f*bpm/60.0f) {
        t->clock = t->end_beat;
        t->phase = RT_FINISHED;
        return true;
    }
    return false;
}

static void submit_onset(rhythm_trainer_t *t, float at, float bpm,
                         unsigned tolerance_ms)
{
    size_t best = RT_MAX_NOTES;
    float best_error = 1e9f;
    float q_ms = 60000.0f / bpm;
    for (size_t i = t->first; i < t->end; ++i) {
        rt_note_t *n = &t->notes[i];
        if (n->rest || n->hit) continue;
        float error = (at - n->start) * q_ms;
        /* Avoid assigning a bow stroke to a distant note, or to two notes. */
        float window = fminf(300.0f, fmaxf((float)tolerance_ms,
                                          n->beats * q_ms * 0.45f));
        /* Bound attribution by adjacent onsets, even for a long previous note. */
        if(i>t->first)window=fminf(window,(n->start-t->notes[i-1].start)*q_ms*.49f);
        if(i+1<t->end)window=fminf(window,(t->notes[i+1].start-n->start)*q_ms*.49f);
        if (fabsf(error) <= window && fabsf(error) < best_error) {
            best = i; best_error = fabsf(error);
        }
    }
    if (best < RT_MAX_NOTES) {
        t->notes[best].hit = true;
        t->notes[best].onset_ms = (int32_t)lrintf((at - t->notes[best].start) * q_ms);
    }
}

void rt_onset(rhythm_trainer_t *t, int64_t us, float bpm, unsigned tolerance_ms)
{
    if((t->phase!=RT_COUNT_IN && t->phase!=RT_PLAYING) || !t->last_us || bpm<=0)return;
    float at=t->clock+(float)(us-t->last_us)*bpm/60000000.0f
        -(float)t->offset_ms*bpm/60000.0f;
    submit_onset(t,at,bpm,tolerance_ms);
}

/* Kept for source compatibility; rhythm uses short PCM envelopes explicitly. */
void rt_audio(rhythm_trainer_t *t, int64_t us, float bpm, bool sound, bool valid,
              float hz, float confidence, float rms, float gate, unsigned tolerance_ms)
{
    (void)valid;(void)hz;(void)confidence;(void)rms;(void)gate;
    if(!sound){t->armed=true;return;}
    if(t->armed){rt_onset(t,us,bpm,tolerance_ms);t->armed=false;}
}
