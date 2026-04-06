/* arpeggiator.h
 *
 * Lightweight note arpeggiator helper for MIDIAY.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ARPEGGIATOR_H
#define ARPEGGIATOR_H

#include <stddef.h>

#define ARPEGGIATOR_MAX_NOTES (8)

typedef struct {
  int enabled;
  int step_frames;
  int frame_counter;
  int note_count;
  int step_index;
  int current_note;
  int notes[ARPEGGIATOR_MAX_NOTES];
} arpeggiator_t;

void arpeggiator_init(arpeggiator_t *arp);
void arpeggiator_set_enabled(arpeggiator_t *arp, int enabled);
void arpeggiator_set_step_frames(arpeggiator_t *arp, int step_frames);
void arpeggiator_set_notes(arpeggiator_t *arp, const int *notes, size_t count);
int arpeggiator_tick(arpeggiator_t *arp, int fallback_note);

#endif /* ARPEGGIATOR_H */

