/* echo.c
 *
 * Simple feedback echo helper.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdlib.h>

#include "echo.h"

/* Macros for common operations */
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define CLAMP(x,min,max) MAX(min, MIN(max, x))

echo_t *echo_create(size_t max_delay_length)
{
  echo_t *echo;

  echo = malloc(sizeof(*echo));
  if (echo == NULL)
    return NULL;

  echo->buffer = calloc(max_delay_length, sizeof(echo_sample_t));
  if (echo->buffer == NULL) {
    free(echo);
    return NULL;
  }

  echo->max_size = max_delay_length;
  echo->size     = max_delay_length;
  echo->pos      = 0;

  return echo;
}

void echo_destroy(echo_t *echo)
{
  if (echo == NULL)
    return;

  free(echo->buffer);
  free(echo);
}

echo_sample_t echo_process(echo_t *echo, echo_sample_t sample)
{
  int32_t delayed;
  int32_t mixed;
  int32_t feedback;

  if (echo == NULL || echo->size == 0)
    return sample;

  delayed  = echo->buffer[echo->pos];
  mixed    = (int32_t) sample + (delayed >> 1); /* 50% wet mix */
  feedback = (int32_t) sample + (delayed >> 1); /* 50% feedback */

  echo->buffer[echo->pos] = (echo_sample_t) CLAMP(feedback, INT16_MIN, INT16_MAX);
  echo->pos = (echo->pos + 1) % echo->size;

  return (echo_sample_t) CLAMP(mixed, INT16_MIN, INT16_MAX);
}

void echo_set_delay(echo_t *echo, size_t delay_length)
{
  if (echo == NULL)
    return;

  if (delay_length > echo->max_size)
    delay_length = echo->max_size;

  echo->size = delay_length;
  echo->pos  = 0;
}
