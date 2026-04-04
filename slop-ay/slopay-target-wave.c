/* slopay-target-wave.c
 *
 * WAV file output driver for SlopAY.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include "slopay-target-wave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Number of stereo frames passed to the render callback per iteration.
   882 = 44100 / 50, matching one AY/Z80 frame at 50 Hz. */
#define WAV_RENDER_CHUNK    882
#define WAV_CHANNELS        2
#define WAV_BITS_PER_SAMPLE 16

/*
 * fpack() - serialise values to a file (little-endian multi-byte fields).
 *
 * Format characters:
 *   b   - uint8_t  (1 byte)
 *   H   - uint16_t (2 bytes, little-endian)
 *   I   - uint32_t (4 bytes, little-endian)
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
      b[0] = (uint8_t)(v & 0xFF);
      b[1] = (uint8_t)(v >> 8);
      if (fwrite(b, 1, 2, f) != 2) ok = 0;
      break;
    }
    case 'I': {
      uint32_t v = va_arg(ap, uint32_t);
      b[0] = (uint8_t)(v & 0xFF);
      b[1] = (uint8_t)((v >>  8) & 0xFF);
      b[2] = (uint8_t)((v >> 16) & 0xFF);
      b[3] = (uint8_t)((v >> 24) & 0xFF);
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

/* Write a 44-byte RIFF/WAVE/fmt /data header.  The RIFF chunk size and data
   chunk size are written as zero placeholders; call wav_finalize() to patch
   them once the total sample count is known. */
static int wav_write_header(FILE *f, int sample_rate)
{
  const uint32_t block_align = WAV_CHANNELS * (WAV_BITS_PER_SAMPLE / 8);
  const uint32_t byte_rate   = (uint32_t)sample_rate * block_align;

  return fpack(f, "4sI4s4sIHHIIHH4sI",
               "RIFF", (uint32_t)0,                       /* chunk size (later) */
               "WAVE",
               "fmt ", (uint32_t)16,                       /* fmt chunk size     */
               (unsigned)1,                                /* PCM = 1            */
               (unsigned)WAV_CHANNELS,
               (uint32_t)sample_rate,
               (uint32_t)byte_rate,
               (unsigned)block_align,
               (unsigned)WAV_BITS_PER_SAMPLE,
               "data", (uint32_t)0);                       /* data size (later)  */
}

/* Seek back and patch the two placeholder sizes. */
static int wav_finalize(FILE *f, uint32_t samples_written)
{
  const uint32_t data_size = samples_written * WAV_CHANNELS * (WAV_BITS_PER_SAMPLE / 8);
  const uint32_t riff_size = 36u + data_size; /* 36 = "WAVE" + fmt chunk + data chunk header */

  if (fseek(f, 4, SEEK_SET) != 0)         return -1;
  if (fpack(f, "I", riff_size) != 0)      return -1;

  if (fseek(f, 40, SEEK_SET) != 0)        return -1;
  if (fpack(f, "I", data_size) != 0)      return -1;

  return 0;
}

/* -------------------------------------------------------------------------
 * Sample conversion
 * ---------------------------------------------------------------------- */

static int16_t float_to_s16(float v)
{
  if (v >  1.0f) v =  1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int16_t)(v * 32767.0f);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int slopay_target_wave_init(slopay_target_wave_t *driver,
                        const char       *filename,
                        int               sample_rate,
                        slopay_render_fn render,
                        void             *userdata)
{
  if (driver == NULL || filename == NULL || render == NULL)
    return -1;

  memset(driver, 0, sizeof(*driver));
  driver->sample_rate = sample_rate;
  driver->render      = render;
  driver->userdata    = userdata;

  driver->file = fopen(filename, "wb");
  if (driver->file == NULL) {
    perror(filename);
    return -1;
  }

  if (wav_write_header(driver->file, sample_rate) != 0) {
    fprintf(stderr, "ay_wave_driver: failed to write WAV header\n");
    fclose(driver->file);
    driver->file = NULL;
    return -1;
  }

  return 0;
}

int slopay_target_wave_render_all(slopay_target_wave_t *driver, uint32_t total_samples)
{
  /* Two local buffers kept on the stack; each is under 4 kB. */
  float     float_buf[WAV_RENDER_CHUNK * WAV_CHANNELS];
  int16_t   s16_buf[WAV_RENDER_CHUNK * WAV_CHANNELS];

  uint32_t  done     = 0;
  int       last_pct = -1;

  if (driver == NULL || driver->file == NULL || driver->render == NULL)
    return -1;

  while (done < total_samples) {
    uint32_t  chunk    = WAV_RENDER_CHUNK;
    size_t    expected;
    uint32_t  i;
    int       pct;

    if (done + chunk > total_samples)
      chunk = total_samples - done;

    /* Ask the emulator for a chunk of stereo float samples. */
    driver->render(driver->userdata, float_buf, chunk);

    /* Convert float → int16_t (signed, little-endian on LE hosts). */
    for (i = 0; i < chunk * WAV_CHANNELS; i++)
      s16_buf[i] = float_to_s16(float_buf[i]);

    expected = (size_t)chunk * WAV_CHANNELS;
    if (fwrite(s16_buf, sizeof(int16_t), expected, driver->file) != expected) {
      fprintf(stderr, "ay_wave_driver: write error\n");
      return -1;
    }

    done += chunk;
    driver->samples_written = done;

    /* Print a simple percentage progress bar to stderr. */
    pct = (int)((uint64_t)done * 100 / total_samples);
    if (pct != last_pct) {
      fprintf(stderr, "\rEncoding WAV: %3d%%", pct);
      fflush(stderr);
      last_pct = pct;
    }
  }

  fprintf(stderr, "\n");
  return (int)done;
}

void slopay_target_wave_cleanup(slopay_target_wave_t *driver)
{
  if (driver == NULL || driver->file == NULL)
    return;

  if (wav_finalize(driver->file, driver->samples_written) != 0)
    fprintf(stderr, "ay_wave_driver: warning – could not finalize WAV header\n");

  fclose(driver->file);
  driver->file = NULL;
}
