/* polyblep.h
 *
 * PolyBLEP oscillator helper declarations.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef POLYBLEP_H
#define POLYBLEP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef float pbflt_t;

/* PolyBLEP oscillator structure */
typedef struct {
  int     enabled;     /* Non-zero if enabled */
  int     filter;      /* Enable filtering */
  pbflt_t phase;       /* Current phase (0.0 to 1.0) */
  pbflt_t freq;        /* Frequency in Hz */
  pbflt_t sample_rate; /* Sample rate */
  pbflt_t phase_inc;   /* Phase */
  pbflt_t pw;          /* Pulse width (0.0 to 1.0) */
} polyblep_osc_t;

void polyblep_init(polyblep_osc_t *osc, pbflt_t frequency, pbflt_t sample_rate);

/* Enable oscillator */
void polyblep_enable(polyblep_osc_t *osc, int enabled);

/* Set oscillator frequency */
void polyblep_set_freq(polyblep_osc_t *osc, pbflt_t frequency);

/* Set pulse width (0.0 to 1.0) */
void polyblep_set_pw(polyblep_osc_t *osc, pbflt_t pulse_width);

/* Generate a single sample */
pbflt_t polyblep_sample(polyblep_osc_t *osc);

#ifdef __cplusplus
}
#endif

#endif /* POLYBLEP_H */
