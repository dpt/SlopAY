/* slopay-target-midi.h
 *
 * Minimal Standard MIDI File (SMF) writer for piano-roll style exports.
 */

#ifndef SLOPAY_TARGET_MIDI_H
#define SLOPAY_TARGET_MIDI_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
  FILE *file;
  long track_size_offset;
  uint32_t track_bytes;
} slopay_target_midi_t;

int slopay_target_midi_init(slopay_target_midi_t *driver, const char *filename);
int slopay_target_midi_note_on(slopay_target_midi_t *driver,
                               uint32_t delta_ticks,
                               uint8_t channel,
                               uint8_t note,
                               uint8_t velocity);
int slopay_target_midi_note_off(slopay_target_midi_t *driver,
                                uint32_t delta_ticks,
                                uint8_t channel,
                                uint8_t note,
                                uint8_t velocity);
int slopay_target_midi_cleanup(slopay_target_midi_t *driver, uint32_t final_delta_ticks);

#endif /* SLOPAY_TARGET_MIDI_H */

