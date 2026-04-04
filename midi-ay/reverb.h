/* reverb.h
 *
 * Simple delay-based reverb declarations.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef REVERB_H
#define REVERB_H

#include <stddef.h>
#include <stdint.h>

typedef int16_t reverb_sample_t;

typedef struct {
  reverb_sample_t *buffer;
  size_t           max_size;
  size_t           size;
  size_t           pos;
} reverb_t;

reverb_t *reverb_create(size_t delay_length);
void reverb_destroy(reverb_t *rev);
reverb_sample_t reverb_process(reverb_t *rev, reverb_sample_t sample);
void reverb_set_delay(reverb_t *rev, size_t delay_length);

#endif /* REVERB_H */
