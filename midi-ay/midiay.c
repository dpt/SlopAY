/* midiay.c
 *
 * Interactive AY shell with macOS MIDI and audio I/O.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include "slopay-chip.h"
#include "slopay-target-macos.h"
#include "notes/arpeggiator.h"
#include "notes/chords.h"
#include "oscillators/polyblep.h"
#include "effects/reverb.h"
#include "effects/echo.h"

/* ----------------------------------------------------------------------- */

/* MIDI */
#define MIDI_A4                     (69) /* 440Hz */

/* MIDI Status Bytes */
#define MIDI_NOTE_OFF             (0x80)
#define MIDI_NOTE_ON              (0x90)
#define MIDI_CONTROL_CHANGE       (0xB0)
#define MIDI_PROGRAM_CHANGE       (0xC0)
#define MIDI_CHANNEL_PRESSURE     (0xD0)
#define MIDI_PITCH_BEND           (0xE0)

/* MIDI Controllers */
#define MIDI_MOD_WHEEL               (1)
#define MIDI_VOLUME                  (7)

/* MIDI Constants */
#define MIDI_MAX_VALUE             (127)

/* ----------------------------------------------------------------------- */

/* AY configuration */
#define SAMPLE_RATE              (44100) /* Standard CD-quality sample rate */
#define CLOCK_FREQ             (1773450) /* ZX Spectrum 128K AY clock frequency in Hz */

#define MAX_POLYPHONY      (AY_CHANNELS)
#define REVERB_CHANNELS              (2)

#define MIDI_DEBUG                   (0) /* Set to 1 to print MIDI events to console */

/* ----------------------------------------------------------------------- */

#define PLAY_MODE_NOTE_MS_DEFAULT  (200)
#define PLAY_MODE_NOTE_MS_MIN       (50)
#define PLAY_MODE_NOTE_MS_MAX     (2000)
#define PLAY_MODE_NOTE_MS_STEP      (50)
#define PLAY_MODE_ARP_STEP_MS_DEFAULT (60)
#define PLAY_MODE_ARP_STEP_MS_MIN     (20)
#define PLAY_MODE_ARP_STEP_MS_MAX    (400)
#define PLAY_MODE_ARP_STEP_MS_STEP    (20)
#define PLAY_MODE_ARP_SLICE_MS        (20)
#define PLAY_MODE_DEFAULT_OCT        (4)
#define PLAY_MODE_MIN_OCT            (0)
#define PLAY_MODE_MAX_OCT            (8)

#define MIDI_NOTE_COUNT            (128)

/* ----------------------------------------------------------------------- */

#define NELEMS(x) (sizeof(x) / sizeof((x)[0]))

#define DIV_NEAREST(a,b) (((a) + (b) / 2) / (b))

/* ----------------------------------------------------------------------- */

typedef struct {
  slopay_chip_t        *ay;
  polyblep_osc_t        oscs[MAX_POLYPHONY];
  reverb_t             *rev[2];
  echo_t               *echo[2];
  slopay_target_macos_t audio_driver;
  int                   output_polyblep;
  int                   channel_to_note[MAX_POLYPHONY];
  int                   mixerstate;
  int                   envshape;
  MIDIClientRef         midiClient;
  MIDIPortRef           inputPort;
  int                   env_period;
  int                   stereo_mode;
  size_t                reverb_delay;
  size_t                echo_delay;
  int                   chord_enabled;
  chord_t               chord_type;
  int                   arp_enabled;
  int                   arp_step_ms;
  arpeggiator_t         midi_arp;
  int                   midi_arp_samples_until_step;
  int                   midi_arp_current_note;
  uint8_t               channel_volume_level[MAX_POLYPHONY];
  int                   channel_envelope_enabled[MAX_POLYPHONY];
  uint8_t               midi_note_held[MIDI_NOTE_COUNT];
  int                   midi_chord_active[MIDI_NOTE_COUNT];
  int                   midi_chord_notes[MIDI_NOTE_COUNT][CHORD_MAX_NOTES];
} midiay_state_t;

static midiay_state_t g_state = {
  .stereo_mode  = SLOPAY_CHIP_STEREO_MODE_MONO,
  .reverb_delay = 0,
  .echo_delay   = 0,
  .chord_enabled = 0,
  .chord_type    = CHORD_TYPE_MAJOR,
  .arp_enabled   = 0,
  .arp_step_ms   = PLAY_MODE_ARP_STEP_MS_DEFAULT,
  .midi_arp_samples_until_step = 1,
  .midi_arp_current_note = -1,
  .channel_volume_level = { 15, 15, 15 },
  .channel_envelope_enabled = { 0, 0, 0 }
};

/* Parse a direct AY register write command in the form "<reg> <value>". */
static int parse_register_write(const char *input, int *reg_out, int *val_out)
{
  char *endptr;
  long  reg;
  long  val;

  reg = strtol(input, &endptr, 10);
  if (endptr == input)
    return 0;

  while (*endptr == ' ' || *endptr == '\t')
    endptr++;

  val = strtol(endptr, &endptr, 10);
  if (reg < 0 || reg > 15 || val < 0 || val > 255)
    return 0;

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n')
    endptr++;
  if (*endptr != '\0')
    return 0;

  *reg_out = (int)reg;
  *val_out = (int)val;
  return 1;
}

/* Parse the channel volume command payload. */
static int parse_channel_volume_command(const char *input, int *value_out)
{
  char cmd[32];
  const char *p = input;
  char *endptr;
  long  value;

  while (*p == ' ' || *p == '\t')
    p++;

  {
    size_t i = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      if (i + 1 >= sizeof(cmd))
        return 0;
      cmd[i++] = *p++;
    }
    cmd[i] = '\0';
  }

  if (strcasecmp(cmd, "g") != 0 && strcasecmp(cmd, "channel-volume") != 0)
    return 0;

  endptr = (char *)p;
  while (*endptr == ' ' || *endptr == '\t')
    endptr++;

  value = strtol(endptr, &endptr, 10);
  if (value < 0 || value > MIDI_MAX_VALUE)
    return 0;

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n')
    endptr++;
  if (*endptr != '\0')
    return 0;

  *value_out = (int)value;
  return 1;
}

/* Parse the master volume command payload. */
static int parse_master_volume_command(const char *input, int *value_out)
{
  char cmd[32];
  const char *p = input;
  char *endptr;
  char *value_start;
  long  value;

  while (*p == ' ' || *p == '\t')
    p++;

  {
    size_t i = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      if (i + 1 >= sizeof(cmd))
        return 0;
      cmd[i++] = *p++;
    }
    cmd[i] = '\0';
  }

  if (strcasecmp(cmd, "v") != 0 && strcasecmp(cmd, "volume") != 0)
    return 0;

  endptr = (char *)p;
  while (*endptr == ' ' || *endptr == '\t')
    endptr++;

  value_start = endptr;

  value = strtol(endptr, &endptr, 10);
  if (endptr == value_start)
    return 0;
  if (value < 0 || value > 100)
    return 0;

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n')
    endptr++;
  if (*endptr != '\0')
    return 0;

  *value_out = (int)value;
  return 1;
}

/* Parse an effect disable command of the form "<key> 0" or "<name> 0". */
static int parse_effect_disable_command(const char *input, const char effect, const char *name)
{
  char cmd[32];
  const char *p = input;

  while (*p == ' ' || *p == '\t')
    p++;

  {
    size_t i = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      if (i + 1 >= sizeof(cmd))
        return 0;
      cmd[i++] = *p++;
    }
    cmd[i] = '\0';
  }

  if (strcasecmp(cmd, (char[]){ (char)tolower((unsigned char)effect), '\0' }) != 0 &&
      (name == NULL || strcasecmp(cmd, name) != 0))
    return 0;

  while (*p == ' ' || *p == '\t')
    p++;

  if (*p != '0')
    return 0;
  p++;

  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    p++;

  return (*p == '\0');
}

/* Parse per-channel envelope enable/disable input. */
static int parse_envelope_channel_command(const char *input, int *channel_out, int *enabled_out)
{
  char        cmd[32];
  const char *p = input;
  char       *endptr;
  long        enabled;
  int         channel;

  while (*p == ' ' || *p == '\t')
    p++;

  {
    size_t i = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      if (i + 1 >= sizeof(cmd))
        return 0;
      cmd[i++] = *p++;
    }
    cmd[i] = '\0';
  }

  if (strcasecmp(cmd, "u") != 0 && strcasecmp(cmd, "envelope") != 0)
    return 0;

  while (*p == ' ' || *p == '\t')
    p++;

  if (*p >= '0' && *p <= '2') {
    channel = *p - '0';
    p++;
  } else {
    const int c = toupper((unsigned char)*p);
    if (c < 'A' || c > 'C')
      return 0;
    channel = c - 'A';
    p++;
  }

  while (*p == ' ' || *p == '\t')
    p++;

  enabled = strtol(p, &endptr, 10);
  if (endptr == p || (enabled != 0 && enabled != 1))
    return 0;

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n')
    endptr++;
  if (*endptr != '\0')
    return 0;

  *channel_out = channel;
  *enabled_out = (int)enabled;
  return 1;
}

/* Map a play-mode keyboard key to its semitone offset. */
static int play_mode_semitone_for_key(const int key)
{
  switch (key) {
  case 'A': return 0;  /* C */
  case 'W': return 1;  /* C# */
  case 'S': return 2;  /* D */
  case 'E': return 3;  /* D# */
  case 'D': return 4;  /* E */
  case 'F': return 5;  /* F */
  case 'T': return 6;  /* F# */
  case 'G': return 7;  /* G */
  case 'Y': return 8;  /* G# */
  case 'H': return 9;  /* A */
  case 'U': return 10; /* A# */
  case 'J': return 11; /* B */
  default:
    return -1;
  }
}

/* Put stdin into raw mode for single-key play-mode input. */
static int terminal_enable_raw_mode(struct termios *saved)
{
  struct termios raw;

  if (saved == NULL)
    return -1;
  if (tcgetattr(STDIN_FILENO, saved) != 0)
    return -1;

  raw = *saved;
  raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;

  return tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* Restore terminal mode saved before entering play mode. */
static void terminal_restore_mode(const struct termios *saved)
{
  if (saved != NULL)
    tcsetattr(STDIN_FILENO, TCSANOW, saved);
}

/* Read a single key from stdin while in raw mode. */
static int read_single_key(int *key)
{
  unsigned char ch;
  const ssize_t n = read(STDIN_FILENO, &ch, 1);

  if (n != 1)
    return -1;
  *key = (int)ch;
  return 0;
}

static void repl_print_help_table(void);
static void key_hold(int note);
static void key_release(int note);
static void key_release_all(void);

/* Apply the cached volume/envelope state to one AY channel register. */
static void apply_channel_volume_register(const int ch)
{
  const uint8_t reg_value = (uint8_t)((g_state.channel_volume_level[ch] & 0x0Fu) |
                                      (g_state.channel_envelope_enabled[ch] ? AY_VOLUME_USE_ENVELOPE_BIT : 0x00u));
  slopay_chip_write_register(g_state.ay,
                             (slopay_chip_reg_t)(AY_REG_CHANNEL_A_VOLUME + ch),
                             reg_value);
}

/* Enable or disable envelope-controlled volume for one AY channel. */
static void set_channel_envelope_enabled(const int ch, const int enabled)
{
  if (ch < 0 || ch >= MAX_POLYPHONY)
    return;

  g_state.channel_envelope_enabled[ch] = enabled ? 1 : 0;
  apply_channel_volume_register(ch);
  printf("Envelope %s for channel %c\n",
         g_state.channel_envelope_enabled[ch] ? "enabled" : "disabled",
         (char)('A' + ch));
}

/* Convert the arpeggiator step duration from milliseconds to samples. */
static int midi_arp_step_samples(void)
{
  int step_samples = (SAMPLE_RATE * g_state.arp_step_ms) / 1000;
  if (step_samples <= 0)
    step_samples = 1;
  return step_samples;
}

/* Release the note currently sounding from the MIDI arpeggiator. */
static void midi_arp_release_current_note(void)
{
  if (g_state.midi_arp_current_note >= 0) {
    key_release(g_state.midi_arp_current_note);
    g_state.midi_arp_current_note = -1;
  }
}

/* Rebuild the MIDI arpeggiator note pool from held roots/chords. */
static void midi_arp_refresh_notes(void)
{
  int notes[ARPEGGIATOR_MAX_NOTES];
  int count = 0;

  for (int root = 0; root < MIDI_NOTE_COUNT; root++) {
    if (!g_state.midi_note_held[root])
      continue;

    if (g_state.midi_chord_active[root]) {
      for (int i = 0; i < CHORD_MAX_NOTES && count < ARPEGGIATOR_MAX_NOTES; i++) {
        const int note = g_state.midi_chord_notes[root][i];
        int duplicate = 0;

        for (int j = 0; j < count; j++)
          if (notes[j] == note) {
            duplicate = 1;
            break;
          }

        if (!duplicate)
          notes[count++] = note;
      }
    } else if (count < ARPEGGIATOR_MAX_NOTES) {
      int duplicate = 0;

      for (int j = 0; j < count; j++)
        if (notes[j] == root) {
          duplicate = 1;
          break;
        }

      if (!duplicate)
        notes[count++] = root;
    }
  }

  arpeggiator_set_notes(&g_state.midi_arp, notes, (size_t)count);
}

/* Advance the MIDI arpeggiator by one step and update sounding note. */
static void midi_arp_tick_once(void)
{
  int next_note;

  if (!g_state.arp_enabled || g_state.midi_arp.note_count <= 0) {
    midi_arp_release_current_note();
    return;
  }

  next_note = arpeggiator_tick(&g_state.midi_arp, g_state.midi_arp.notes[0]);
  if (next_note == g_state.midi_arp_current_note)
    return;

  midi_arp_release_current_note();
  key_hold(next_note);
  g_state.midi_arp_current_note = next_note;
}

/* Reconcile active notes when MIDI arpeggiator mode is toggled. */
static void midi_arp_sync_enabled_state(void)
{
  arpeggiator_set_enabled(&g_state.midi_arp, g_state.arp_enabled);
  arpeggiator_set_step_frames(&g_state.midi_arp, 1);
  g_state.midi_arp_samples_until_step = midi_arp_step_samples();

  if (!g_state.arp_enabled) {
    midi_arp_release_current_note();
    for (int root = 0; root < MIDI_NOTE_COUNT; root++) {
      if (!g_state.midi_note_held[root])
        continue;

      if (g_state.midi_chord_active[root]) {
        for (int i = 0; i < CHORD_MAX_NOTES; i++)
          key_hold(g_state.midi_chord_notes[root][i]);
      } else
        key_hold(root);
    }
    return;
  }

  for (int ch = 0; ch < MAX_POLYPHONY; ch++) {
    if (g_state.channel_to_note[ch] != 0)
      key_release(g_state.channel_to_note[ch]);
  }
  midi_arp_refresh_notes();
  midi_arp_tick_once();
}

/* ----------------------------------------------------------------------- */

/* Render one audio buffer and drive time-based MIDI arpeggiator ticks. */
static void render_audio(void *userdata, float *output, uint32_t frames)
{
  int16_t              sample[REVERB_CHANNELS];
  pbflt_t              o1, o2, o3;
  float                mixed;
  slopay_chip_sample_t pair;

  (void)userdata;

  for (uint32_t i = 0; i < frames; i++) {
    if (g_state.arp_enabled) {
      g_state.midi_arp_samples_until_step--;
      if (g_state.midi_arp_samples_until_step <= 0) {
        midi_arp_tick_once();
        g_state.midi_arp_samples_until_step += midi_arp_step_samples();
      }
    }

    if (g_state.output_polyblep) {
      o1 = polyblep_sample(&g_state.oscs[0]);
      o2 = polyblep_sample(&g_state.oscs[1]);
      o3 = polyblep_sample(&g_state.oscs[2]);
      mixed = (o1 + o2 + o3) / 3.0f * 32767.0f * 0.10f;
      sample[0] = sample[1] = (int16_t)lroundf(mixed);
    } else {
      pair = slopay_chip_get_sample(g_state.ay);
      sample[0] = (int16_t)(uint16_t)(pair & 0xFFFFu);
      sample[1] = (int16_t)(uint16_t)((pair >> 16) & 0xFFFFu);
    }

    for (int ch = 0; ch < REVERB_CHANNELS; ch++) {
      sample[ch] = echo_process(g_state.echo[ch], sample[ch]);
      sample[ch] = reverb_process(g_state.rev[ch], sample[ch]);
      output[i * REVERB_CHANNELS + ch] = sample[ch] / 32767.0f; /* Scale to -1.0 to 1.0 */
    }
  }
}

/* ----------------------------------------------------------------------- */

/* Start a note on the first available synth voice. */
static void key_hold(int note)
{
  int ch;

  /* Audio Notes:
   *
   * On ZX Spectrum 128K models the AY chip is clocked at 1.77345 MHz (half CPU
   * clock). The output frequency is equal to the AY clock frequency over 16
   * further divided by the respective pitch register value.
   *
   * e.g.
   * A4 would be (1773450 / 16) / 252 = ~439.84Hz
   * C4 would be (1773450 / 16) / 424 = ~261.41Hz
   */

  /* Find a spare channel */
  for (ch = 0; ch < MAX_POLYPHONY; ch++)
    if (g_state.channel_to_note[ch] == 0)
      break;
  if (ch == MAX_POLYPHONY)
    return; /* no spare channels */

  /* Convert the MIDI note number into a frequency */
  const float freq = 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
  const int ifreq = (int)lroundf(freq);
  if (ifreq <= 0)
    return;

  /* Configure some registers to generate sound */
  const int ay_period = DIV_NEAREST(CLOCK_FREQ, ifreq) / 16; /* C4 should be approx 424 here */
  /* printf("ifreq=%d, ay_period=%d\n", ifreq, ay_period); */
  slopay_chip_write_register(g_state.ay, AY_REG_CHANNEL_A_FINE_PITCH + ch * 2, ay_period & 0xFF);
  slopay_chip_write_register(g_state.ay, AY_REG_CHANNEL_A_COARSE_PITCH + ch * 2, ay_period >> 8);
  slopay_chip_write_register(g_state.ay, AY_REG_ENVELOPE_SHAPE, g_state.envshape);

  /* slopay_chip_write_register(ay, AY_REG_NOISE_PITCH, note & 31); // 0..31 */

  g_state.mixerstate |= AY_MIXER_NO_TONE_A << ch; /* inverted sense here */
  /* mixerstate |= AY_MIXER_NO_NOISE_A << ch; // inverted sense here */
  slopay_chip_write_register(g_state.ay, AY_REG_MIXER, ~g_state.mixerstate); /* enable */

  polyblep_set_freq(&g_state.oscs[ch], freq);
  polyblep_enable(&g_state.oscs[ch], 1);

  g_state.channel_to_note[ch] = note;
}

/* Stop a currently playing note if present. */
static void key_release(int note)
{
  int ch;

  /* Find the channel playing the note we want to stop */
  for (ch = 0; ch < MAX_POLYPHONY; ch++)
    if (g_state.channel_to_note[ch] == note)
      break;
  if (ch == MAX_POLYPHONY)
    return; /* not playing that note */

  g_state.mixerstate &= ~(AY_MIXER_NO_TONE_A << ch);
  g_state.mixerstate &= ~(AY_MIXER_NO_NOISE_A << ch);
  slopay_chip_write_register(g_state.ay, AY_REG_MIXER, ~g_state.mixerstate);

  polyblep_enable(&g_state.oscs[ch], 0);

  g_state.channel_to_note[ch] = 0;
}

/* Stop all active voices and clear MIDI/chord/arpeggiator tracking. */
static void key_release_all(void)
{
  int ch;

  for (ch = 0; ch < MAX_POLYPHONY; ch++) {
    g_state.mixerstate &= ~(AY_MIXER_NO_TONE_A << ch);
    g_state.mixerstate &= ~(AY_MIXER_NO_NOISE_A << ch);
    slopay_chip_write_register(g_state.ay, AY_REG_MIXER, ~g_state.mixerstate);

    polyblep_enable(&g_state.oscs[ch], 0);

    g_state.channel_to_note[ch] = 0;
  }

  for (int note = 0; note < MIDI_NOTE_COUNT; note++)
    g_state.midi_chord_active[note] = 0;

  for (int note = 0; note < MIDI_NOTE_COUNT; note++)
    g_state.midi_note_held[note] = 0;

  arpeggiator_set_notes(&g_state.midi_arp, NULL, 0);
  g_state.midi_arp_current_note = -1;
  g_state.midi_arp_samples_until_step = midi_arp_step_samples();
}

/* Set all channel volumes from a normalized MIDI volume value. */
static void setchannelvol(float midi_volume)
{
  int ch;
  const uint8_t ay_volume = (uint8_t)lroundf(midi_volume * 15.0f);

  printf("Setting channel volumes to %.2f\n", midi_volume);
  for (ch = 0; ch < MAX_POLYPHONY; ch++) {
    g_state.channel_volume_level[ch] = ay_volume;
    apply_channel_volume_register(ch);
    polyblep_set_pw(&g_state.oscs[ch], midi_volume);
  }
}

/* Cycle the AY envelope shape register. */
static void setenvshape(void)
{
  g_state.envshape = (g_state.envshape + 1) % 16;
  printf("Setting envelope shape to %d\n", g_state.envshape);
}

/* Cycle the AY envelope period register value. */
static void setenvperiod(void)
{
  g_state.env_period = (g_state.env_period + 16) % 256;
  printf("Setting envelope period to %d\n", g_state.env_period);
  slopay_chip_write_register(g_state.ay, AY_REG_ENVELOPE_FINE_DURATION, 0);
  slopay_chip_write_register(g_state.ay, AY_REG_ENVELOPE_COARSE_DURATION, g_state.env_period);
}

/* Cycle stereo routing mode and apply it to the chip. */
static void cyclestereomode(void)
{
  const char *name;

  g_state.stereo_mode = (g_state.stereo_mode + 1) % 3;
  slopay_chip_set_stereo_mode(g_state.ay, (slopay_chip_stereo_mode_t)g_state.stereo_mode);

  if (g_state.stereo_mode == SLOPAY_CHIP_STEREO_MODE_ACB)
    name = "acb";
  else if (g_state.stereo_mode == SLOPAY_CHIP_STEREO_MODE_ABC)
    name = "abc";
  else
    name = "mono";

  printf("Setting stereo mode to %s\n", name);
}

/* Cycle the reverb delay through preset values. */
static void cyclereverbdelay(void)
{
  g_state.reverb_delay = (g_state.reverb_delay >> 1);
  if (g_state.reverb_delay < SAMPLE_RATE / 16)
    g_state.reverb_delay = SAMPLE_RATE;

  for (int ch = 0; ch < REVERB_CHANNELS; ch++)
    reverb_set_delay(g_state.rev[ch], g_state.reverb_delay);

  printf("Set reverb delay to %zu samples (%.2f ms)\n", g_state.reverb_delay, (double)g_state.reverb_delay / SAMPLE_RATE * 1000);
}

/* Disable reverb and reset its state buffers. */
static void disablereverb(void)
{
  g_state.reverb_delay = 0;
  for (int ch = 0; ch < REVERB_CHANNELS; ch++) {
    reverb_set_delay(g_state.rev[ch], 0);
    g_state.rev[ch]->size = 0;
    g_state.rev[ch]->pos  = 0;
  }

  printf("Reverb disabled\n");
}

/* Cycle the echo delay through preset values. */
static void cycleechodelay(void)
{
  g_state.echo_delay = (g_state.echo_delay >> 1);
  if (g_state.echo_delay < SAMPLE_RATE / 16)
    g_state.echo_delay = SAMPLE_RATE;

  for (int ch = 0; ch < REVERB_CHANNELS; ch++)
    echo_set_delay(g_state.echo[ch], g_state.echo_delay);

  printf("Set echo delay to %zu samples (%.2f ms)\n", g_state.echo_delay, (double)g_state.echo_delay / SAMPLE_RATE * 1000);
}

/* Disable echo and reset its state buffers. */
static void disableecho(void)
{
  g_state.echo_delay = 0;
  for (int ch = 0; ch < REVERB_CHANNELS; ch++) {
    echo_set_delay(g_state.echo[ch], 0);
    g_state.echo[ch]->size = 0;
    g_state.echo[ch]->pos  = 0;
  }

  printf("Echo disabled\n");
}

/* Toggle global chord mode and refresh MIDI arpeggiator notes. */
static void togglechordmode(void)
{
  g_state.chord_enabled = !g_state.chord_enabled;
  if (g_state.arp_enabled) {
    midi_arp_refresh_notes();
    midi_arp_tick_once();
  }
}

/* Cycle the active chord type and refresh MIDI arpeggiator notes. */
static void cyclechordtype(void)
{
  g_state.chord_type = (chord_t)((g_state.chord_type + 1) % CHORD_TYPE__LIMIT);
  printf("MIDI chord type: %s\n", chord_name(g_state.chord_type));
  if (g_state.arp_enabled) {
    midi_arp_refresh_notes();
    midi_arp_tick_once();
  }
}

/* ----------------------------------------------------------------------- */


/* Handle a MIDI note-on event with chord and arpeggiator logic. */
static void midi_note_on(int note)
{
  if (note < 0 || note >= MIDI_NOTE_COUNT)
    return;

  g_state.midi_note_held[note] = 1;

  if (!g_state.chord_enabled) {
    g_state.midi_chord_active[note] = 0;
    if (!g_state.arp_enabled)
      key_hold(note);
    else {
      midi_arp_refresh_notes();
      midi_arp_tick_once();
      g_state.midi_arp_samples_until_step = midi_arp_step_samples();
    }
    return;
  }

  if (g_state.midi_chord_active[note]) {
    if (!g_state.arp_enabled)
      for (int i = 0; i < CHORD_MAX_NOTES; i++)
        key_release(g_state.midi_chord_notes[note][i]);
  }

  chord_build(note, g_state.chord_type, g_state.midi_chord_notes[note]);
  if (!g_state.arp_enabled)
    for (int i = 0; i < CHORD_MAX_NOTES; i++)
      key_hold(g_state.midi_chord_notes[note][i]);
  else {
    midi_arp_refresh_notes();
    midi_arp_tick_once();
    g_state.midi_arp_samples_until_step = midi_arp_step_samples();
  }

  g_state.midi_chord_active[note] = 1;
}

/* Handle a MIDI note-off event with chord and arpeggiator logic. */
static void midi_note_off(int note)
{
  if (note < 0 || note >= MIDI_NOTE_COUNT)
    return;

  g_state.midi_note_held[note] = 0;

  if (g_state.arp_enabled) {
    g_state.midi_chord_active[note] = 0;
    midi_arp_refresh_notes();
    midi_arp_tick_once();
    g_state.midi_arp_samples_until_step = midi_arp_step_samples();
    return;
  }

  if (g_state.midi_chord_active[note]) {
    for (int i = CHORD_MAX_NOTES - 1; i >= 0; i--)
      key_release(g_state.midi_chord_notes[note][i]);
    g_state.midi_chord_active[note] = 0;
    return;
  }

  key_release(note);
}

/* Decode incoming CoreMIDI packets and dispatch supported message types. */
static void midiMessageCallback(const MIDIPacketList *pktList, void *refCon, void *connRefCon) {
  const MIDIPacket *packet = pktList->packet;
  (void)refCon;
  (void)connRefCon;

  for (UInt32 i = 0; i < pktList->numPackets; i++) {
#if MIDI_DEBUG
    {
      printf("MIDI: ");
      for (int j = 0; j < packet->length && j < 8; j++)
        printf("%02x ", packet->data[j]);
      printf("\n");
    }
#endif

    /* Parse basic MIDI message types */
    if (packet->length > 0) {
      const uint8_t status = packet->data[0];
      const uint8_t command = status & 0xF0u;
      const uint8_t channel = status & 0x0Fu;
      const uint8_t data1 = packet->length > 1 ? packet->data[1] : 0;
      const uint8_t data2 = packet->length > 2 ? packet->data[2] : 0;

      switch (command) {
      case MIDI_NOTE_OFF: /* Note Off */
        printf("  Note Off: Channel %d, Note %d, Velocity %d\n",
               channel + 1, data1, data2);
        midi_note_off(data1);
        break;
      case MIDI_NOTE_ON: /* Note On */
        printf("  Note On: Channel %d, Note %d, Velocity %d\n",
               channel + 1, data1, data2);
        if (data2)
          midi_note_on(data1);
        else
          midi_note_off(data1);
        break;
      case MIDI_CONTROL_CHANGE: /* Control Change */
        printf("  Control Change: Channel %d, Controller %d, Value %d\n",
               channel + 1, data1, data2);
        if (data1 == MIDI_MOD_WHEEL) { /* mod */
          if (data2 == MIDI_MAX_VALUE)
            g_state.output_polyblep = !g_state.output_polyblep;
          printf("use_polyblep=%d\n", g_state.output_polyblep);
        }
        if (data1 == MIDI_VOLUME) { /* volume */
          const float midi_volume = (float)data2 / (float)MIDI_MAX_VALUE;
          printf("midi_volume=%.2f\n", midi_volume);
          setchannelvol(midi_volume);
        }
        break;
      case MIDI_PROGRAM_CHANGE: /* Program Change */
        printf("  Program Change: Channel %d, Program %d\n",
               channel + 1, data1);
        break;
      case MIDI_CHANNEL_PRESSURE: /* Channel Pressure */
        printf("  Channel Pressure: Channel %d, Pressure %d\n",
               channel + 1, data1);
        break;
      case MIDI_PITCH_BEND: /* Pitch Bend */
        printf("  Pitch Bend: Channel %d, Value %d\n",
               channel + 1, (data2 << 7) | data1);
        break;
      default:
        printf("  Unknown MIDI command: %02x\n", command);
        break;
      }
    }

    packet = MIDIPacketNext(packet);
  }
}

/* Create MIDI client/input port and connect all available sources. */
static int setupMIDI(void)
{
  const ItemCount sourceCount = MIDIGetNumberOfSources();
  ItemCount i;
  OSStatus status;

  CFStringRef clientName = CFSTR("MIDI Monitor");
  status = MIDIClientCreate(clientName, NULL, NULL, &g_state.midiClient);
  if (status != noErr)
    return -1;

  CFStringRef endpointName = CFSTR("MIDI Input");
  status = MIDIInputPortCreate(g_state.midiClient, endpointName, midiMessageCallback, NULL, &g_state.inputPort);
  if (status != noErr) {
    MIDIClientDispose(g_state.midiClient);
    g_state.midiClient = 0;
    return -1;
  }

  for (i = 0; i < sourceCount; i++)
    MIDIPortConnectSource(g_state.inputPort, MIDIGetSource(i), NULL);

  return 0;
}

/* Dispose MIDI input resources created by setupMIDI(). */
static void teardownMIDI(void)
{
  if (g_state.inputPort != 0) {
    MIDIPortDispose(g_state.inputPort);
    g_state.inputPort = 0;
  }
  if (g_state.midiClient != 0) {
    MIDIClientDispose(g_state.midiClient);
    g_state.midiClient = 0;
  }
}

/* Print current play-mode state including chord and arpeggiator settings. */
static void play_mode_print_status(int play_octave,
                                   int note_ms,
                                   int chord_enabled,
                                   chord_t chord_type,
                                   int arp_enabled,
                                   int arp_step_ms)
{
  printf("\rOctave: %d  Hold: %dms  Chord: %s", play_octave, note_ms, chord_enabled ? "on" : "off");
  if (chord_enabled)
    printf(" (%s)", chord_name(chord_type));
  printf("  Arp: %s", arp_enabled ? "on" : "off");
  if (arp_enabled)
    printf(" (%dms)", arp_step_ms);
  printf("\n");
}

/* Print a compact chord status line before each REPL prompt. */
static void repl_print_chord_status(void)
{
  printf("MIDI chord mode: %s", g_state.chord_enabled ? "on" : "off");
  if (g_state.chord_enabled)
    printf(" (%s)", chord_name(g_state.chord_type));
  printf("\n");
}


/* ----------------------------------------------------------------------- */

/* Run the interactive single-key play mode loop. */
static void run_play_mode(void)
{
  struct termios saved;
  int            play_octave   = PLAY_MODE_DEFAULT_OCT;
  int            note_ms       = PLAY_MODE_NOTE_MS_DEFAULT;
  arpeggiator_t  arp;

  arpeggiator_init(&arp);
  arpeggiator_set_step_frames(&arp, g_state.arp_step_ms / PLAY_MODE_ARP_SLICE_MS);
  arpeggiator_set_enabled(&arp, g_state.arp_enabled);

  printf("Play mode: A S D F G H J = C D E F G A B, W E T Y U = sharps\n");
  printf("           Z/X = octave down/up, [/] = shorter/longer hold, C = toggle 3-note chords, M = cycle chord type\n");
  printf("           V = toggle arpeggiator, K/L = slower/faster arp, Space = stop all, Q = return\n");
  play_mode_print_status(play_octave,
                         note_ms,
                         g_state.chord_enabled,
                         g_state.chord_type,
                         g_state.arp_enabled,
                         g_state.arp_step_ms);

  if (terminal_enable_raw_mode(&saved) != 0) {
    printf("Failed to enter raw input mode\n");
    return;
  }

  for (;;) {
    int key;
    int semitone;
    int note;

    if (read_single_key(&key) != 0)
      break;

    if (key >= 'a' && key <= 'z')
      key -= ('a' - 'A');

    if (key == 'Q')
      break;

    switch (key) {
    case ' ':
      key_release_all();
      continue;
    case 'Z':
      if (play_octave > PLAY_MODE_MIN_OCT) {
        play_octave--;
        play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      }
      continue;
    case 'X':
      if (play_octave < PLAY_MODE_MAX_OCT) {
        play_octave++;
        play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      }
      continue;
    case 'C':
      g_state.chord_enabled = !g_state.chord_enabled;
      play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      continue;
    case 'M':
      g_state.chord_type = (chord_t)((g_state.chord_type + 1) % CHORD_TYPE__LIMIT);
      play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      continue;
    case 'V':
      g_state.arp_enabled = !g_state.arp_enabled;
      arpeggiator_set_enabled(&arp, g_state.arp_enabled);
      midi_arp_sync_enabled_state();
      play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      continue;
    case 'K':
      if (g_state.arp_step_ms < PLAY_MODE_ARP_STEP_MS_MAX) {
        g_state.arp_step_ms += PLAY_MODE_ARP_STEP_MS_STEP;
        if (g_state.arp_step_ms > PLAY_MODE_ARP_STEP_MS_MAX)
          g_state.arp_step_ms = PLAY_MODE_ARP_STEP_MS_MAX;
        arpeggiator_set_step_frames(&arp, g_state.arp_step_ms / PLAY_MODE_ARP_SLICE_MS);
        midi_arp_sync_enabled_state();
        play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      }
      continue;
    case 'L':
      if (g_state.arp_step_ms > PLAY_MODE_ARP_STEP_MS_MIN) {
        g_state.arp_step_ms -= PLAY_MODE_ARP_STEP_MS_STEP;
        if (g_state.arp_step_ms < PLAY_MODE_ARP_STEP_MS_MIN)
          g_state.arp_step_ms = PLAY_MODE_ARP_STEP_MS_MIN;
        arpeggiator_set_step_frames(&arp, g_state.arp_step_ms / PLAY_MODE_ARP_SLICE_MS);
        midi_arp_sync_enabled_state();
        play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      }
      continue;
    case '[':
      if (note_ms > PLAY_MODE_NOTE_MS_MIN) {
        note_ms -= PLAY_MODE_NOTE_MS_STEP;
        if (note_ms < PLAY_MODE_NOTE_MS_MIN)
          note_ms = PLAY_MODE_NOTE_MS_MIN;
        play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      }
      continue;
    case ']':
      if (note_ms < PLAY_MODE_NOTE_MS_MAX) {
        note_ms += PLAY_MODE_NOTE_MS_STEP;
        if (note_ms > PLAY_MODE_NOTE_MS_MAX)
          note_ms = PLAY_MODE_NOTE_MS_MAX;
        play_mode_print_status(play_octave, note_ms, g_state.chord_enabled, g_state.chord_type, g_state.arp_enabled, g_state.arp_step_ms);
      }
      continue;
    default:
      break;
    }

    semitone = play_mode_semitone_for_key(key);
    if (semitone < 0)
      continue;

    note = (play_octave + 1) * 12 + semitone;

    {
      int notes[CHORD_MAX_NOTES];
      int note_count = 1;

      notes[0] = note;
      if (g_state.chord_enabled) {
        chord_build(note, g_state.chord_type, notes);
        note_count = CHORD_MAX_NOTES;
      }

      if (!g_state.arp_enabled) {
        for (int i = 0; i < note_count; i++)
          key_hold(notes[i]);

        usleep((useconds_t)note_ms * 1000u);

        for (int i = note_count - 1; i >= 0; i--)
          key_release(notes[i]);
      } else {
        int active_note = -1;
        int remaining_ms = note_ms;

        arpeggiator_set_notes(&arp, notes, (size_t)note_count);
        while (remaining_ms > 0) {
          const int arp_note = arpeggiator_tick(&arp, notes[0]);
          const int sleep_ms = (remaining_ms < PLAY_MODE_ARP_SLICE_MS)
                               ? remaining_ms
                               : PLAY_MODE_ARP_SLICE_MS;

          if (arp_note != active_note) {
            if (active_note >= 0)
              key_release(active_note);
            key_hold(arp_note);
            active_note = arp_note;
          }

          usleep((useconds_t)sleep_ms * 1000u);
          remaining_ms -= sleep_ms;
        }

        if (active_note >= 0)
          key_release(active_note);
      }
    }
  }

  key_release_all();
  terminal_restore_mode(&saved);
  printf("\nExited play mode\n");
}

/* ----------------------------------------------------------------------- */

typedef struct {
  int    key;
  void (*action)(void);
  int    is_quit;
} repl_key_command_t;

typedef struct {
  const char *word;
  int         key;
} repl_word_alias_t;

static const repl_key_command_t repl_key_commands[] = {
  { 'Q', NULL,             1 },
  { 'H', repl_print_help_table, 0 },
  { 'C', togglechordmode,  0 },
  { 'M', cyclechordtype,   0 },
  { 'S', setenvshape,      0 },
  { 'P', run_play_mode,    0 },
  { 'O', setenvperiod,     0 },
  { 'R', cyclereverbdelay, 0 },
  { 'E', cycleechodelay,   0 },
  { 'X', cyclestereomode,  0 },
  { '.', key_release_all,  0 }
};

static const repl_word_alias_t repl_word_aliases[] = {
  { "help",            'H' },
  { "play",            'P' },
  { "chord",           'C' },
  { "chord-type",      'M' },
  { "envelope-shape",  'S' },
  { "envelope-period", 'O' },
  { "reverb",          'R' },
  { "echo",            'E' },
  { "stereo",          'X' },
  { "stereo-mode",     'X' },
  { "stop",            '.' },
  { "quit",            'Q' }
};

static int repl_lookup_command_key(const char *word)
{
  if (word == NULL || *word == '\0')
    return 0;

  for (size_t i = 0; i < NELEMS(repl_word_aliases); i++)
    if (strcasecmp(word, repl_word_aliases[i].word) == 0)
      return repl_word_aliases[i].key;

  return 0;
}

/* ----------------------------------------------------------------------- */

/* Print the command-mode help table. */
static void repl_print_help_table(void)
{
  printf("+---------------------------+---------------------------------+\n");
  printf("| Command                   | Action                          |\n");
  printf("+---------------------------+---------------------------------+\n");
  printf("| <reg> <val>               | Write AY register (e.g. 0 128)  |\n");
  printf("| p, play                   | Enter play mode                 |\n");
  printf("| h, help                   | Show this help table            |\n");
  printf("| c, chord                  | Toggle chord mode               |\n");
  printf("| m, chord-type             | Cycle chord type                |\n");
  printf("| u, envelope <ch> <0|1>    | Envelope off/on for A-C or 0-2  |\n");
  printf("| v, volume <0-100>         | Set master volume percent       |\n");
  printf("| g, channel-volume <0-127> | Set channel volume (MIDI scale) |\n");
  printf("| s, envelope-shape         | Cycle envelope shape            |\n");
  printf("| o, envelope-period        | Cycle envelope period           |\n");
  printf("| r, reverb / reverb 0      | Cycle/disable reverb            |\n");
  printf("| e, echo / echo 0          | Cycle/disable echo              |\n");
  printf("| x, stereo / stereo-mode   | Cycle stereo (mono/abc/acb)     |\n");
  printf("| ., stop                   | Stop all notes                  |\n");
  printf("| q, quit                   | Quit                            |\n");
  printf("+---------------------------+---------------------------------+\n");
  printf("Play mode and MIDI input share global chord settings.\n");
}

/* ----------------------------------------------------------------------- */

/* Run the line-oriented REPL command loop. */
static void repl(void)
{
  char input[100];
  char command_word[32];
  const char *scan;
  size_t command_len;
  int  cmd;
  int  reg;
  int  val;
  int  env_ch;
  int  env_enabled;
  int  volume_value;
  int  master_volume_value;

  repl_print_help_table();

  for (;;) {
    int handled = 0;

    repl_print_chord_status();
    printf("Command> ");
    if (!fgets(input, sizeof(input), stdin))
      break;

    scan = input;
    while (*scan == ' ' || *scan == '\t')
      scan++;
    command_len = 0;
    while (scan[command_len] != '\0' && scan[command_len] != ' ' && scan[command_len] != '\t' &&
           scan[command_len] != '\r' && scan[command_len] != '\n') {
      if (command_len + 1 < sizeof(command_word))
        command_word[command_len] = (char)tolower((unsigned char)scan[command_len]);
      command_len++;
    }
    if (command_len >= sizeof(command_word))
      command_len = sizeof(command_word) - 1;
    command_word[command_len] = '\0';

    cmd = input[0];
    if (cmd >= 'a' && cmd <= 'z')
      cmd -= ('a' - 'A');

    {
      const int alias_key = repl_lookup_command_key(command_word);
      if (alias_key != 0)
        cmd = alias_key;
    }

    if (!handled && parse_master_volume_command(input, &master_volume_value)) {
      slopay_chip_set_volume(g_state.ay, master_volume_value);
      printf("Set master volume to %d%%\n", master_volume_value);
      handled = 1;
    }

    if (!handled && parse_channel_volume_command(input, &volume_value)) {
      setchannelvol((float)volume_value / (float)MIDI_MAX_VALUE);
      handled = 1;
    }

    if (!handled && parse_envelope_channel_command(input, &env_ch, &env_enabled)) {
      set_channel_envelope_enabled(env_ch, env_enabled);
      handled = 1;
    }

    if (!handled && parse_register_write(input, &reg, &val)) {
      slopay_chip_write_register(g_state.ay, (slopay_chip_reg_t)reg, (uint8_t)val);
      printf("Wrote %d to register %d\n", val, reg);
      handled = 1;
    }

    if (!handled && parse_effect_disable_command(input, 'R', "reverb")) {
      disablereverb();
      handled = 1;
    }

    if (!handled && parse_effect_disable_command(input, 'E', "echo")) {
      disableecho();
      handled = 1;
    }

    if (!handled)
      for (size_t i = 0; i < NELEMS(repl_key_commands); i++) {
        if (cmd == repl_key_commands[i].key) {
          if (repl_key_commands[i].is_quit)
            return;
          if (repl_key_commands[i].action != NULL)
            repl_key_commands[i].action();
          handled = 1;
          break;
        }
      }

    if (!handled)
      printf("Invalid input. Use 'h'/'help' for help or 'q'/'quit' to quit\n");
  }
}

/* ----------------------------------------------------------------------- */

/* Initialize subsystems, run REPL, and perform cleanup. */
int main(void)
{
  int ch;

  printf("MIDIAY: AY-3-8912 Emulator\n");
  printf("==========================\n");

  printf("Sample rate: %dHz\n", SAMPLE_RATE);
  printf("Clock speed: %.2fMHz\n", (double)CLOCK_FREQ / 1000000.0);

  /* Initialize AY state */
  g_state.ay = slopay_chip_create(CLOCK_FREQ, SAMPLE_RATE);
  if (g_state.ay == NULL) {
    printf("Failed to create AY\n");
    goto failure;
  }

  /* Set defaults */
  g_state.mixerstate = 0;
  slopay_chip_write_register(g_state.ay, AY_REG_MIXER, AY_MIXER_ALL_OFF);

  for (ch = 0; ch < AY_CHANNELS; ch++) {
    slopay_chip_write_register(g_state.ay, AY_REG_CHANNEL_A_VOLUME + ch, 15); /* Set channel volume (max) */
    polyblep_init(&g_state.oscs[ch], 440, SAMPLE_RATE);  /* 440Hz at 44.1kHz sample rate */
  }
  slopay_chip_write_register(g_state.ay, AY_REG_ENVELOPE_SHAPE, 0);
  slopay_chip_write_register(g_state.ay, AY_REG_ENVELOPE_FINE_DURATION, 0);
  slopay_chip_write_register(g_state.ay, AY_REG_ENVELOPE_COARSE_DURATION, 4);
  slopay_chip_set_stereo_mode(g_state.ay, SLOPAY_CHIP_STEREO_MODE_ABC); /* default stereo mode */
  slopay_chip_set_volume(g_state.ay, 10); /* 10% master volume */
  g_state.stereo_mode = SLOPAY_CHIP_STEREO_MODE_ABC;

  arpeggiator_init(&g_state.midi_arp);
  arpeggiator_set_step_frames(&g_state.midi_arp, 1);
  arpeggiator_set_enabled(&g_state.midi_arp, g_state.arp_enabled);
  g_state.midi_arp_samples_until_step = midi_arp_step_samples();
  g_state.midi_arp_current_note = -1;

  for (ch = 0; ch < REVERB_CHANNELS; ch++) {
    g_state.rev[ch] = reverb_create(SAMPLE_RATE);
    if (g_state.rev[ch] == NULL) {
      printf("Failed to create reverb\n");
      goto failure;
    }
    g_state.rev[ch]->size = 0; /* Reverb OFF by default (bypass path in reverb_process). */
    g_state.rev[ch]->pos  = 0;

    g_state.echo[ch] = echo_create(SAMPLE_RATE);
    if (g_state.echo[ch] == NULL) {
      printf("Failed to create echo\n");
      goto failure;
    }
    g_state.echo[ch]->size = 0; /* Echo OFF by default (bypass path in echo_process). */
    g_state.echo[ch]->pos  = 0;
  }

  if (setupMIDI() != 0) {
    printf("Failed to initialize MIDI\n");
    goto failure;
  }

  /* Initialize CoreAudio */
  OSStatus result = slopay_target_macos_init(&g_state.audio_driver, SAMPLE_RATE * 1, render_audio, NULL); /* 1 second max */
  if (result != noErr) {
    printf("Failed to initialize audio\n");
    goto failure;
  }

  /* Start playback */
  result = slopay_target_macos_start(&g_state.audio_driver);
  if (result != noErr) {
    printf("Failed to start audio playback\n");
    slopay_target_macos_cleanup(&g_state.audio_driver);
    goto failure;
  }

  repl();

  /* Stop and cleanup */
  slopay_target_macos_stop(&g_state.audio_driver);
  slopay_target_macos_cleanup(&g_state.audio_driver);
  teardownMIDI();
  for (ch = REVERB_CHANNELS - 1; ch >= 0; ch--) {
    echo_destroy(g_state.echo[ch]);
    reverb_destroy(g_state.rev[ch]);
  }
  slopay_chip_destroy(g_state.ay);

  printf("Test completed\n");

  exit(EXIT_SUCCESS);

failure:
  slopay_target_macos_cleanup(&g_state.audio_driver);
  teardownMIDI();
  for (ch = REVERB_CHANNELS - 1; ch >= 0; ch--) {
    if (g_state.echo[ch] != NULL) {
      echo_destroy(g_state.echo[ch]);
      g_state.echo[ch] = NULL;
    }
    if (g_state.rev[ch] != NULL) {
      reverb_destroy(g_state.rev[ch]);
      g_state.rev[ch] = NULL;
    }
  }
  if (g_state.ay != NULL) {
    slopay_chip_destroy(g_state.ay);
    g_state.ay = NULL;
  }
  exit(EXIT_FAILURE);
}


