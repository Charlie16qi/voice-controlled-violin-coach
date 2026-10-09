#include "note_utils.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *SHARP_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

static int natural_semitone(char letter)
{
    switch ((char)toupper((unsigned char)letter)) {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
        default: return -100;
    }
}

static void compact_ascii(
    const char *source,
    char *output,
    size_t capacity
)
{
    size_t write_index = 0;

    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!source) return;

    for (
        size_t index = 0;
        source[index] != '\0'
        && write_index + 1 < capacity;
        ++index
    ) {
        unsigned char value =
            (unsigned char)source[index];

        if (isspace(value)) continue;

        if (
            value >= 'a'
            && value <= 'z'
        ) {
            value = (unsigned char)toupper(value);
        }

        output[write_index++] = (char)value;
    }

    output[write_index] = '\0';
}

bool note_parse(
    const char *text,
    char *canonical,
    size_t capacity
)
{
    if (!text || !canonical || capacity < 4) {
        return false;
    }

    char compact[32];
    compact_ascii(
        text,
        compact,
        sizeof(compact)
    );

    if (!compact[0]) return false;

    int base = natural_semitone(compact[0]);
    if (base < 0) return false;

    size_t index = 1;
    int accidental = 0;

    if (compact[index] == '#') {
        accidental = 1;
        ++index;
    } else if (
        compact[index] == 'B'
        && isdigit(
            (unsigned char)compact[index + 1]
        )
    ) {
        /*
         * Db4等降号输入在compact后会变成DB4。
         * 只有“音名字母之后、数字之前”的B才解释为flat。
         */
        accidental = -1;
        ++index;
    }

    if (!isdigit((unsigned char)compact[index])) {
        return false;
    }

    int octave = 0;
    while (isdigit((unsigned char)compact[index])) {
        octave =
            octave * 10
            + (compact[index] - '0');
        ++index;
    }

    if (compact[index] != '\0') {
        return false;
    }

    int midi =
        (octave + 1) * 12
        + base
        + accidental;

    if (
        midi < 0
        || midi > 127
    ) {
        return false;
    }

    return note_from_midi(
        midi,
        canonical,
        capacity
    );
}

int note_to_midi(const char *note)
{
    char canonical[NOTE_NAME_CAPACITY];
    if (
        !note_parse(
            note,
            canonical,
            sizeof(canonical)
        )
    ) {
        return -1;
    }

    int base = natural_semitone(canonical[0]);
    size_t index = 1;

    if (canonical[index] == '#') {
        ++base;
        ++index;
    }

    int octave = 0;
    while (isdigit((unsigned char)canonical[index])) {
        octave =
            octave * 10
            + (canonical[index] - '0');
        ++index;
    }

    return (octave + 1) * 12 + base;
}

bool note_from_midi(
    int midi,
    char *note,
    size_t capacity
)
{
    if (
        midi < 0
        || midi > 127
        || !note
        || capacity < 4
    ) {
        return false;
    }

    int pitch_class = midi % 12;
    int octave = midi / 12 - 1;

    int written = snprintf(
        note,
        capacity,
        "%s%d",
        SHARP_NAMES[pitch_class],
        octave
    );

    return (
        written > 0
        && (size_t)written < capacity
    );
}

bool note_step(
    const char *note,
    int semitones,
    char *output,
    size_t capacity
)
{
    int midi = note_to_midi(note);
    if (midi < 0) return false;

    midi += semitones;

    if (
        midi < NOTE_MIN_MIDI
        || midi > NOTE_MAX_MIDI
    ) {
        return false;
    }

    return note_from_midi(
        midi,
        output,
        capacity
    );
}

float note_frequency(const char *note)
{
    int midi = note_to_midi(note);
    if (midi < 0) return 0.0f;

    return 440.0f
        * powf(
            2.0f,
            ((float)midi - 69.0f) / 12.0f
        );
}

bool note_is_supported(const char *note)
{
    int midi = note_to_midi(note);
    return (
        midi >= NOTE_MIN_MIDI
        && midi <= NOTE_MAX_MIDI
    );
}
