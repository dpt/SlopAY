/* slopay_target_wave.h
 *
 * WAV file output driver for AY player
 */

#ifndef SLOPAY_TARGET_WAVE_H
#define SLOPAY_TARGET_WAVE_H

#include <stdint.h>
#include <stdio.h>

typedef void (*slopay_target_wave_render_fn)(void *userdata,
                                             float *output,
                                             uint32_t frames);

typedef struct slopay_target_wave {
  FILE *file;
  int sample_rate;
  slopay_target_wave_render_fn render;
  void *userdata;
  uint32_t samples_written;
} slopay_target_wave_t;

int slopay_target_wave_init(slopay_target_wave_t *driver,
                            const char *filename,
                            int sample_rate,
                            slopay_target_wave_render_fn render,
                            void *userdata);

int slopay_target_wave_render_all(slopay_target_wave_t *driver, uint32_t total_samples);
void slopay_target_wave_cleanup(slopay_target_wave_t *driver);

#endif /* SLOPAY_TARGET_WAVE_H */
