/* slopay-target-midi.c
 *
 * Minimal Standard MIDI File writer for piano-roll style exports.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include "slopay-target-midi.h"

#include <stdarg.h>
#include <string.h>

#define SLOPAY_MIDI_PPQ      50
#define SLOPAY_MIDI_TEMPO_US 1000000u /* 60 BPM => 1 tick = 1 frame at 50 Hz */

/*
 * fpack() - serialise values to a file (big-endian multi-byte fields).
 *
 * Format characters:
 *   b   - uint8_t  (1 byte)
 *   H   - uint16_t (2 bytes, big-endian)
 *   I   - uint32_t (4 bytes, big-endian)
 *   Ns  - const char * (N raw bytes; N is a decimal count prefix)
 *
 * Returns 0 on success, -1 on any write error.
 */
static int fpack(FILE *f, const char *fmt, ...)
{
  va_list ap;
  const char *p = fmt;
  uint8_t b[4];
  int ok = 1;

  va_start(ap, fmt);
  while (*p && ok) {
    int count = 1;
    if (*p >= '0' && *p <= '9') {
      count = 0;
      while (*p >= '0' && *p <= '9')
        count = count * 10 + (*p++ - '0');
    }
    switch (*p++) {
    case 'b': {
      unsigned v = va_arg(ap, unsigned);
      b[0] = (uint8_t)v;
      if (fwrite(b, 1, 1, f) != 1) ok = 0;
      break;
    }
    case 'H': {
      unsigned v = va_arg(ap, unsigned);
      b[0] = (uint8_t)(v >> 8);
      b[1] = (uint8_t)(v & 0xFF);
      if (fwrite(b, 1, 2, f) != 2) ok = 0;
      break;
    }
    case 'I': {
      uint32_t v = va_arg(ap, uint32_t);
      b[0] = (uint8_t)(v >> 24);
      b[1] = (uint8_t)(v >> 16);
      b[2] = (uint8_t)(v >> 8);
      b[3] = (uint8_t)(v & 0xFF);
      if (fwrite(b, 1, 4, f) != 4) ok = 0;
      break;
    }
    case 's': {
      const char *src = va_arg(ap, const char *);
      if (fwrite(src, 1, (size_t)count, f) != (size_t)count) ok = 0;
      break;
    }
    default:
      ok = 0;
      break;
    }
  }
  va_end(ap);

  return ok ? 0 : -1;
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
  if (driver == NULL || filename == NULL)
    return -1;

  memset(driver, 0, sizeof(*driver));
  driver->file = fopen(filename, "wb");
  if (driver->file == NULL)
    return -1;

  /* MThd chunk + MTrk chunk ID */
  if (fpack(driver->file, "4sIHHH4s",
            "MThd", (uint32_t)6,
            (unsigned)0,              /* format 0       */
            (unsigned)1,              /* 1 track        */
            (unsigned)SLOPAY_MIDI_PPQ,
            "MTrk") != 0) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  /* Capture position of MTrk chunk size, then write placeholder */
  driver->track_size_offset = ftell(driver->file);
  if (driver->track_size_offset < 0 || fpack(driver->file, "I", (uint32_t)0) != 0) {
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
      fpack(driver->file, "I", driver->track_bytes) != 0 ||
      fseek(driver->file, end_pos, SEEK_SET) != 0) {
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  fclose(driver->file);
  driver->file = NULL;
  return 0;
}

