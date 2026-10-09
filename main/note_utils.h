#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 软件支持范围：C3~C8。标准小提琴通常从G3起；C3~F#3用于扩展/测试。 */
#define NOTE_MIN_MIDI 48
#define NOTE_MAX_MIDI 108
#define NOTE_NAME_CAPACITY 8

bool note_parse(
    const char *text,
    char *canonical,
    size_t capacity
);

int note_to_midi(const char *note);

bool note_from_midi(
    int midi,
    char *note,
    size_t capacity
);

bool note_step(
    const char *note,
    int semitones,
    char *output,
    size_t capacity
);

float note_frequency(const char *note);

bool note_is_supported(const char *note);
