/* chords.c
 *
 * Chord helpers for MIDIAY play mode and MIDI input expansion.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include "chords.h"

struct chord_data {
  const char *name;
  int         intervals[CHORD_MAX_NOTES];
};

static const struct chord_data chord_database[CHORD_TYPE__LIMIT] = {
  { "maj",  { 0, 4,  7 } }, /* major */
  { "min",  { 0, 3,  7 } }, /* minor */
  { "sus4", { 0, 5,  7 } }, /* sus4 */
  { "sus2", { 0, 2,  7 } }, /* sus2 */
  { "dim",  { 0, 3,  6 } }, /* diminished */
  { "aug",  { 0, 4,  8 } }, /* augmented */
  { "5",    { 0, 7, 12 } }  /* power (root, fifth, octave) */
};

const char *chord_name(chord_t chord_type)
{
  if (chord_type < 0 || chord_type >= CHORD_TYPE__LIMIT)
    return "?";
  return chord_database[chord_type].name;
}

static int fit_note(int note)
{
  /* Wrap notes into MIDI range [0, 127] by octave shifts (12 semitones). */
  if (note > 127)
    note -= 12 * ((note - 116) / 12);
  if (note < 0)
    note += 12 * ((11 - note) / 12);
  return note;
}

void chord_build(int     root_note,
                 chord_t chord_type,
                 int     notes_out[CHORD_MAX_NOTES])
{
  if (chord_type < 0 || chord_type >= CHORD_TYPE__LIMIT)
    return;

  for (int i = 0; i < CHORD_MAX_NOTES; i++)
    notes_out[i] = fit_note(root_note + chord_database[chord_type].intervals[i]);
}
