/* arpeggiator.c
 *
 * Lightweight note arpeggiator helper for MIDIAY.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include "arpeggiator.h"

#include <string.h>

static int wrap_midi_note(int note)
{
  if (note > 127)
    note -= 12 * ((note - 116) / 12);
  if (note < 0)
    note += 12 * ((11 - note) / 12);
  return note;
}

void arpeggiator_init(arpeggiator_t *arp)
{
  if (arp == NULL)
    return;

  memset(arp, 0, sizeof(*arp));
  arp->step_frames = 1;
  arp->current_note = -1;
}

void arpeggiator_set_enabled(arpeggiator_t *arp, int enabled)
{
  if (arp == NULL)
    return;

  arp->enabled = enabled ? 1 : 0;
}

void arpeggiator_set_step_frames(arpeggiator_t *arp, int step_frames)
{
  if (arp == NULL)
    return;

  arp->step_frames = (step_frames > 0) ? step_frames : 1;
  if (arp->frame_counter > arp->step_frames)
    arp->frame_counter = arp->step_frames;
}

void arpeggiator_set_notes(arpeggiator_t *arp, const int *notes, size_t count)
{
  size_t n;

  if (arp == NULL)
    return;

  if (notes == NULL || count == 0) {
    arp->note_count = 0;
    arp->step_index = 0;
    arp->current_note = -1;
    arp->frame_counter = 0;
    return;
  }

  n = (count > ARPEGGIATOR_MAX_NOTES) ? ARPEGGIATOR_MAX_NOTES : count;
  for (size_t i = 0; i < n; i++)
    arp->notes[i] = wrap_midi_note(notes[i]);

  arp->note_count = (int)n;
  arp->step_index = 0;
  arp->current_note = arp->notes[0];
  arp->frame_counter = arp->step_frames;
}

int arpeggiator_tick(arpeggiator_t *arp, int fallback_note)
{
  if (arp == NULL)
    return wrap_midi_note(fallback_note);

  if (!arp->enabled || arp->note_count <= 0)
    return wrap_midi_note(fallback_note);

  if (arp->current_note < 0)
    arp->current_note = arp->notes[0];

  if (arp->frame_counter <= 0) {
    arp->step_index = (arp->step_index + 1) % arp->note_count;
    arp->current_note = arp->notes[arp->step_index];
    arp->frame_counter = arp->step_frames;
  }

  arp->frame_counter--;
  return arp->current_note;
}

