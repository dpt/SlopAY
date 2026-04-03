/* slopay_target_wave.c
 *
 * WAV file output driver for AY player
 *
 * Writes a standard RIFF/WAV file: 44.1 kHz (or whatever sample_rate is),
 * 2-channel, 16-bit signed little-endian PCM.
 *
 * NOTE: Writing int16_t samples with fwrite() assumes a little-endian host.
 * All modern Apple hardware (x86-64 and ARM64) is little-endian, matching
 * the WAV format requirement.
 */

#include "slopay_target_wave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Number of stereo frames passed to the render callback per iteration.
   882 = 44100 / 50, matching one AY/Z80 frame at 50 Hz. */
#define WAV_RENDER_CHUNK    882
#define WAV_CHANNELS        2
#define WAV_BITS_PER_SAMPLE 16

/* -------------------------------------------------------------------------
 * WAV header helpers (little-endian writes, no alignment assumptions)
 * ---------------------------------------------------------------------- */

static int wav_write_u16le(FILE *f, uint16_t v)
{
  uint8_t b[2];
  b[0] = (uint8_t)(v & 0xFF);
  b[1] = (uint8_t)(v >> 8);
  return fwrite(b, 1, 2, f) == 2 ? 0 : -1;
}

static int wav_write_u32le(FILE *f, uint32_t v)
{
  uint8_t b[4];
  b[0] = (uint8_t)(v & 0xFF);
  b[1] = (uint8_t)((v >>  8) & 0xFF);
  b[2] = (uint8_t)((v >> 16) & 0xFF);
  b[3] = (uint8_t)((v >> 24) & 0xFF);
  return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

/* Write a 44-byte RIFF/WAVE/fmt /data header.  The RIFF chunk size and data
   chunk size are written as zero placeholders; call wav_finalize() to patch
   them once the total sample count is known. */
static int wav_write_header(FILE *f, int sample_rate)
{
  const uint32_t block_align = WAV_CHANNELS * (WAV_BITS_PER_SAMPLE / 8);
  const uint32_t byte_rate   = (uint32_t)sample_rate * block_align;

  if (fwrite("RIFF", 1, 4, f) != 4)        return -1; /* chunk id          */
  if (wav_write_u32le(f, 0))                return -1; /* chunk size (later) */
  if (fwrite("WAVE", 1, 4, f) != 4)        return -1; /* WAVE marker        */
  if (fwrite("fmt ", 1, 4, f) != 4)        return -1; /* fmt  sub-chunk     */
  if (wav_write_u32le(f, 16))              return -1; /* fmt  chunk size    */
  if (wav_write_u16le(f, 1))               return -1; /* PCM = 1            */
  if (wav_write_u16le(f, WAV_CHANNELS))    return -1; /* channels           */
  if (wav_write_u32le(f, (uint32_t)sample_rate)) return -1;
  if (wav_write_u32le(f, byte_rate))       return -1;
  if (wav_write_u16le(f, (uint16_t)block_align)) return -1;
  if (wav_write_u16le(f, WAV_BITS_PER_SAMPLE))   return -1;
  if (fwrite("data", 1, 4, f) != 4)        return -1; /* data sub-chunk     */
  if (wav_write_u32le(f, 0))               return -1; /* data size (later)  */

  return 0;
}

/* Seek back and patch the two placeholder sizes. */
static int wav_finalize(FILE *f, uint32_t samples_written)
{
  const uint32_t data_size = samples_written * WAV_CHANNELS * (WAV_BITS_PER_SAMPLE / 8);
  const uint32_t riff_size = 36u + data_size; /* 36 = "WAVE" + fmt chunk + data chunk header */

  if (fseek(f, 4, SEEK_SET) != 0)  return -1;
  if (wav_write_u32le(f, riff_size)) return -1;

  if (fseek(f, 40, SEEK_SET) != 0)  return -1;
  if (wav_write_u32le(f, data_size)) return -1;

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
                        slopay_target_wave_render_fn render,
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
  float   float_buf[WAV_RENDER_CHUNK * WAV_CHANNELS];
  int16_t s16_buf[WAV_RENDER_CHUNK * WAV_CHANNELS];

  uint32_t done     = 0;
  int      last_pct = -1;

  if (driver == NULL || driver->file == NULL || driver->render == NULL)
    return -1;

  while (done < total_samples) {
    uint32_t chunk = WAV_RENDER_CHUNK;
    size_t expected;
    uint32_t i;
    int      pct;

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


