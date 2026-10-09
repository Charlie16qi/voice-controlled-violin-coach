#pragma once

#include "note_utils.h"

typedef enum {
    INTENT_UNKNOWN = 0,
    INTENT_START_SINGLE,
    INTENT_START_SONG,
    INTENT_PAUSE,
    INTENT_CONTINUE,
    INTENT_NEXT,
    INTENT_PREVIOUS,
    INTENT_TEMPO_UP,
    INTENT_TEMPO_DOWN,
    INTENT_TEMPO_RESET,
    INTENT_TEMPO_SET,
    INTENT_MODE_PITCH,
    INTENT_MODE_RHYTHM,
    INTENT_MODE_FULL,
    INTENT_METRONOME_ON,
    INTENT_METRONOME_OFF,
    INTENT_REVIEW_START,
    INTENT_REVIEW_SKIP,
    INTENT_END,
    INTENT_PHRASE_REPEAT,
    INTENT_PHRASE_SLOW_REPEAT,
    INTENT_PHRASE_NEXT,
    INTENT_RESTART,
    INTENT_MODE_FOLLOW,
} intent_kind_t;

typedef struct {
    intent_kind_t kind;
    char note[NOTE_NAME_CAPACITY];
    char song_query[96];
    /*
     * Violin string numbering follows normal notation / MusicXML:
     * 1 = E, 2 = A, 3 = D, 4 = G, 0 = not explicitly specified.
     */
    int string_id;
    float tempo_bpm;  /* INTENT_TEMPO_SET, otherwise 0 */
} intent_result_t;

intent_result_t intent_parse(const char *text);
