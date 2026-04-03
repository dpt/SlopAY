/* slopay-target-midi.c
 *
 * Minimal Standard MIDI File (format 0) writer.
 */

#include "slopay-target-midi.h"

#include <string.h>

#define SLOPAY_MIDI_PPQ      50
#define SLOPAY_MIDI_TEMPO_US 1000000u /* 60 BPM => 1 tick = 1 frame at 50 Hz */

static int midi_write_u16be(FILE *f, uint16_t v)
{
  uint8_t b[2];
  b[0] = (uint8_t)(v >> 8);
  b[1] = (uint8_t)(v & 0xFFu);
  return fwrite(b, 1, 2, f) == 2 ? 0 : -1;
}

static int midi_write_u32be(FILE *f, uint32_t v)
{
  uint8_t b[4];
  b[0] = (uint8_t)(v >> 24);
  b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);
  b[3] = (uint8_t)(v & 0xFFu);
  return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int midi_track_write(slopay_target_midi_t *driver, const uint8_t *data, size_t len)
{
  if (driver == NULL || driver->file == NULL || data == NULL)
    return -1;
  if (fwrite(data, 1, len, driver->file) != len)
    return -1;
  driver->track_bytes += (uint32_t)len;
  return 0;
}

static int midi_track_write_vlq(slopay_target_midi_t *driver, uint32_t value)
{
  uint8_t bytes[5];
  int count = 0;

  bytes[count++] = (uint8_t)(value & 0x7Fu);
  while ((value >>= 7) != 0)
    bytes[count++] = (uint8_t)((value & 0x7Fu) | 0x80u);

  for (int i = count - 1; i >= 0; i--) {
    if (midi_track_write(driver, &bytes[i], 1) != 0)
      return -1;
  }

  return 0;
}

static int midi_track_write_event(slopay_target_midi_t *driver,
                                  uint32_t delta_ticks,
                                  uint8_t status,
                                  uint8_t data1,
                                  uint8_t data2)
{
  const uint8_t payload[3] = { status, data1, data2 };

  if (midi_track_write_vlq(driver, delta_ticks) != 0)
    return -1;
  return midi_track_write(driver, payload, sizeof(payload));
}

int slopay_target_midi_init(slopay_target_midi_t *driver, const char *filename)
{
  static const uint8_t mthd[4] = { 'M', 'T', 'h', 'd' };
  static const uint8_t mtrk[4] = { 'M', 'T', 'r', 'k' };

  if (driver == NULL || filename == NULL)
    return -1;

  memset(driver, 0, sizeof(*driver));
  driver->file = fopen(filename, "wb");
  if (driver->file == NULL)
    return -1;

  if (fwrite(mthd, 1, sizeof(mthd), driver->file) != sizeof(mthd) ||
      midi_write_u32be(driver->file, 6) != 0 ||
      midi_write_u16be(driver->file, 0) != 0 ||
      midi_write_u16be(driver->file, 1) != 0 ||
      midi_write_u16be(driver->file, SLOPAY_MIDI_PPQ) != 0 ||
      fwrite(mtrk, 1, sizeof(mtrk), driver->file) != sizeof(mtrk)) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  driver->track_size_offset = ftell(driver->file);
  if (driver->track_size_offset < 0 || midi_write_u32be(driver->file, 0) != 0) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  /* Delta=0, Set Tempo meta event. */
  if (midi_track_write_vlq(driver, 0) != 0 ||
      midi_track_write(driver, (const uint8_t[]){ 0xFF, 0x51, 0x03 }, 3) != 0 ||
      midi_track_write(driver, (const uint8_t[]){
        (uint8_t)(SLOPAY_MIDI_TEMPO_US >> 16),
        (uint8_t)(SLOPAY_MIDI_TEMPO_US >> 8),
        (uint8_t)(SLOPAY_MIDI_TEMPO_US & 0xFFu)
      }, 3) != 0) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  return 0;
}

int slopay_target_midi_note_on(slopay_target_midi_t *driver,
                               uint32_t delta_ticks,
                               uint8_t channel,
                               uint8_t note,
                               uint8_t velocity)
{
  if (channel > 15 || note > 127 || velocity > 127)
    return -1;

  return midi_track_write_event(driver,
                                delta_ticks,
                                (uint8_t)(0x90u | channel),
                                note,
                                velocity);
}

int slopay_target_midi_note_off(slopay_target_midi_t *driver,
                                uint32_t delta_ticks,
                                uint8_t channel,
                                uint8_t note,
                                uint8_t velocity)
{
  if (channel > 15 || note > 127 || velocity > 127)
    return -1;

  return midi_track_write_event(driver,
                                delta_ticks,
                                (uint8_t)(0x80u | channel),
                                note,
                                velocity);
}

int slopay_target_midi_cleanup(slopay_target_midi_t *driver, uint32_t final_delta_ticks)
{
  long end_pos;

  if (driver == NULL || driver->file == NULL)
    return -1;

  if (midi_track_write_vlq(driver, final_delta_ticks) != 0 ||
      midi_track_write(driver, (const uint8_t[]){ 0xFF, 0x2F, 0x00 }, 3) != 0) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  end_pos = ftell(driver->file);
  if (end_pos < 0 || fseek(driver->file, driver->track_size_offset, SEEK_SET) != 0 ||
      midi_write_u32be(driver->file, driver->track_bytes) != 0 ||
      fseek(driver->file, end_pos, SEEK_SET) != 0) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  fclose(driver->file);
  driver->file = NULL;
  return 0;
}

