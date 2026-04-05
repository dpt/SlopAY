/* echo.h
 *
 * Simple feedback echo helper declarations.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ECHO_H
#define ECHO_H

#include <stddef.h>
#include <stdint.h>

typedef int16_t echo_sample_t;

typedef struct {
  echo_sample_t *buffer;
  size_t         max_size;
  size_t         size;
  size_t         pos;
} echo_t;

echo_t *echo_create(size_t max_delay_length);
void echo_destroy(echo_t *echo);
echo_sample_t echo_process(echo_t *echo, echo_sample_t sample);
void echo_set_delay(echo_t *echo, size_t delay_length);

#endif /* ECHO_H */

