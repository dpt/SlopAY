/* polyblep.c */

#include "polyblep.h"

/* Macros for common operations */
#define PB_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PB_MAX(a,b) ((a) > (b) ? (a) : (b))
#define PB_CLAMP(x,min,max) PB_MAX(min, PB_MIN(max, x))

void polyblep_init(polyblep_osc_t *osc, pbflt_t frequency, pbflt_t sample_rate)
{
  osc->enabled     = 0;
  osc->filter      = 1;
  osc->phase       = 0.0f;
  osc->freq        = frequency;
  osc->sample_rate = sample_rate;
  osc->phase_inc   = frequency / sample_rate;
  osc->pw          = 0.5f;  /* Default 50% duty cycle */
}

void polyblep_enable(polyblep_osc_t *osc, int enabled)
{
  osc->enabled = enabled;
}

void polyblep_set_freq(polyblep_osc_t *osc, pbflt_t frequency)
{
  osc->freq      = frequency;
  osc->phase_inc = frequency / osc->sample_rate;
}

void polyblep_set_pw(polyblep_osc_t *osc, pbflt_t pulse_width)
{
  osc->pw = PB_CLAMP(pulse_width, 0.001f, 0.999f);
}

/* PolyBLEP function for correcting discontinuities */
static pbflt_t polyblep(pbflt_t t, pbflt_t dt)
{
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  }
  if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

pbflt_t polyblep_sample(polyblep_osc_t *osc)
{
  const pbflt_t phase     = osc->phase;
  const pbflt_t pw        = osc->pw;
  const pbflt_t phase_inc = osc->phase_inc;
  pbflt_t sample;

  if (!osc->enabled)
    return 0.0f;

  /* Square wave generation */
  sample = (phase < pw) ? +1.0f : -1.0f;

  if (osc->filter) {
    /* PolyBLEP correction for discontinuities */

    /* Handle rising edge */
    if (phase < phase_inc) {
      pbflt_t t = phase / phase_inc;
      sample += polyblep(t, phase_inc);
    }

    /* Handle falling edge */
    if (phase > 1.0f - phase_inc) {
      pbflt_t t = (phase - (1.0f - phase_inc)) / phase_inc;
      sample -= polyblep(t, phase_inc);
    }
  }

  /* Update phase */
  osc->phase += phase_inc;
  while (osc->phase >= 1.0f)
    osc->phase -= 1.0f;

  return sample;
}

