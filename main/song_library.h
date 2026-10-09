#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "note_utils.h"

typedef struct {
    char note[NOTE_NAME_CAPACITY];
    float beats;
    /* MusicXML/violin metadata. 0 means unspecified. */
    uint8_t string_id;   /* 1=E, 2=A, 3=D, 4=G */
    int8_t finger;       /* 0=open, 1..4 finger, -1=unknown */
    uint16_t measure;    /* 1-based measure number, 0=unknown */
    float beat;          /* 1-based beat within measure, 0=unknown */
} song_event_t;

typedef struct {
    char id[40];
    char title[72];
    uint8_t time_num;
    uint8_t time_den;
    float tempo_bpm;
    float tolerance_cents;
    uint32_t hold_ms;
    size_t count;
    song_event_t *events;
} song_t;

esp_err_t song_library_fetch(
    const char *query,
    song_t *song
);

esp_err_t song_library_fetch_strict(const char *query, song_t *song);

void song_library_free(song_t *song);

uint32_t song_event_required_ms(
    const song_t *song,
    const song_event_t *event
);

/* V10.4: full notated duration, quarter-note BPM based. */
uint32_t song_event_duration_ms(
    const song_t *song,
    const song_event_t *event
);
