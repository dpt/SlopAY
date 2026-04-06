/* chords.h
 *
 * Chord helpers for MIDIAY play mode and MIDI input expansion.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef CHORDS_H
#define CHORDS_H

#define CHORD_MAX_NOTES (3)

typedef enum {
  CHORD_TYPE_MAJOR = 0,
  CHORD_TYPE_MINOR,
  CHORD_TYPE_SUS4,
  CHORD_TYPE_SUS2,
  CHORD_TYPE_DIM,
  CHORD_TYPE_AUG,
  CHORD_TYPE_POWER,
  CHORD_TYPE__LIMIT
} chord_t;

/**
 * Return a human-readable name for a chord type, e.g. "maj" for major chords.
 *
 * \param[in] chord_type Type of chord to get the name for
 * \return A string representing the chord type (e.g. "maj", "min", etc.)
 */
const char *chord_name(chord_t chord_type);

/**
 * Build a chord by adding intervals to the root note. Output notes are wrapped
 * to fit in the MIDI range (0-127).
 *
 * The chord type determines the intervals added to the root note. For example,
 * a major chord adds intervals of 0, 4, and 7 semitones.
 *
 * \param[in]  root_note  MIDI note number for the chord root (0-127, will be
 *                       wrapped if out of range)
 * \param[in]  chord_type Type of chord to build (major, minor, etc.)
 * \param[out] notes_out Array to receive the MIDI note numbers for the chord
 *                       tones (must have space for CHORD_MAX_NOTES)
 */
void chord_build(int     root_note,
                 chord_t chord_type,
                 int     notes_out[CHORD_MAX_NOTES]);

#endif /* CHORDS_H */
