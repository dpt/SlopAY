/* slopay.c
 *
 * AY file viewer and player.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <limits.h>
#include <math.h>

#include "slopay-loader.h"
#include "slopay-z80.h"
#include "slopay-chip.h"
#include "slopay-target-macos.h"
#include "slopay-target-wave.h"
#include "slopay-target-midi.h"

#define AY_BOOT_ADDR (0x0000) /* AY player code is expected to be loaded at this address */
#define AY_ISR_ADDR (0x0038) /* AY player interrupt service routine is expected to be at this address */
#define SLOPAY_DEFAULT_SAMPLE_RATE (44100) /* Standard CD-quality sample rate */
#define Z80_CYCLE_FXP (8) /* Fixed-point precision for Z80 cycle calculations */
#define SLOPAY_BEEPER_ADD_AY_GAIN (0.90f) /* Beeper gain when mixed additively with AY output */
#define SLOPAY_BEEPER_DUCK_AY_GAIN (0.60f) /* Beeper gain when ducking AY output */
#define SLOPAY_MIDI_AY_CHANNELS 3 /* MIDI channels 0-2 correspond to AY channels A-C */
#define SLOPAY_MIDI_BEEPER_VOICE_INDEX 3 /* MIDI channel index for beeper (if enabled) */
#define SLOPAY_MIDI_CHANNELS 4 /* MIDI channels 0-2 for AY, 3 for beeper (if enabled) */
#define SLOPAY_MIDI_TICKS_PER_FRAME 1u /* MIDI ticks per AY frame (assuming 50 Hz frame rate) */
#define SLOPAY_MIDI_VELOCITY 96 /* Default MIDI velocity for note on events (0-127) */

#ifndef SLOPAY_VERSION_STRING
#define SLOPAY_VERSION_STRING "dev"
#endif

typedef enum {
  SLOPAY_BEEPER_MIX_ADD = 0,
  SLOPAY_BEEPER_MIX_DUCK
} slopay_beeper_mix_mode_t;

typedef enum {
  SLOPAY_STEREO_MODE_MONO = 0,
  SLOPAY_STEREO_MODE_ABC,
  SLOPAY_STEREO_MODE_ACB
} slopay_stereo_mode_t;

typedef enum {
  SLOPAY_MACHINE_SPECTRUM = 0,
  SLOPAY_MACHINE_CPC
} slopay_machine_t;

typedef struct {
  const char              *name;
  int                      z80_clock_freq;
  int                      ay_clock_freq;
  int                      interrupt_rate;
} slopay_machine_profile_t;

typedef struct {
  slopay_z80_t             *cpu;
  slopay_chip_t            *ay;
  slopay_target_macos_t     audio_driver;
  uint8_t                   selected_reg;
  unsigned                  total_out_count;
  unsigned                  ay_select_count;
  unsigned                  ay_write_count;
  unsigned                  beeper_out_count;
  unsigned                  beeper_toggle_count;
  unsigned                  other_out_count;
  int                       beeper_level;
  float                     beeper_gain;
  slopay_beeper_mix_mode_t  beeper_mix_mode;
  int                       piano_roll_enabled;
  int                       midi_export_enabled;
  slopay_target_midi_t      midi_driver;
  int                       midi_notes[SLOPAY_MIDI_CHANNELS];
  uint8_t                   midi_beeper_channel;
  uint32_t                  midi_ticks_since_event;
  unsigned                  midi_beeper_toggle_count_last;
  unsigned                  beeper_toggle_count_last;
  int                       ay_clock_freq;
  int                       frame_rate;
  int                       samples_per_frame;
  int                       z80_cycles_per_sample_fxp;
  int                       target_frames;
  int                       played_frames;
  int                       samples_to_next_frame;
  int                       z80_cycle_error_fxp;
  slopay_machine_t          machine;
  uint8_t                   cpc_psg_control;
  uint8_t                   cpc_ppi_port_a;
} slopay_io_t;

static float slopay_clamp_unit(float v)
{
  if (v > 1.0f)
    return 1.0f;
  if (v < -1.0f)
    return -1.0f;
  return v;
}

static volatile sig_atomic_t slopay_stop_requested = 0;

static const char *slopay_beeper_mix_mode_name(slopay_beeper_mix_mode_t mode)
{
  switch (mode) {
  default:
  case SLOPAY_BEEPER_MIX_ADD:  return "add";
  case SLOPAY_BEEPER_MIX_DUCK: return "duck";
  }
}

static const char *slopay_stereo_mode_name(slopay_stereo_mode_t mode)
{
  switch (mode) {
  default:
  case SLOPAY_STEREO_MODE_MONO: return "mono";
  case SLOPAY_STEREO_MODE_ABC:  return "abc";
  case SLOPAY_STEREO_MODE_ACB:  return "acb";
  }
}

static const slopay_machine_profile_t *slopay_machine_profile(slopay_machine_t machine)
{
  static const slopay_machine_profile_t slopay_machine_profiles[] = {
    { "spectrum", 3494400, 1773450, 50 },
    { "cpc",      4000000, 1000000, 50 }
  };

  if (machine > SLOPAY_MACHINE_CPC)
    machine = SLOPAY_MACHINE_SPECTRUM;
  return &slopay_machine_profiles[machine];
}

static const char *slopay_machine_name(slopay_machine_t machine)
{
  return slopay_machine_profile(machine)->name;
}

#define NELEMS(x) (sizeof(x) / sizeof((x)[0]))

static int slopay_parse_beeper_mix_mode(const char               *text,
                                        slopay_beeper_mix_mode_t *out_mode)
{
  static const struct {
    const char              *name;
    slopay_beeper_mix_mode_t mode;
  } mix_mode_map[] = {
    { "add",      SLOPAY_BEEPER_MIX_ADD },
    { "additive", SLOPAY_BEEPER_MIX_ADD },
    { "duck",     SLOPAY_BEEPER_MIX_DUCK },
    { "ducking",  SLOPAY_BEEPER_MIX_DUCK }
  };

  if (text)
    for (size_t i = 0; i < NELEMS(mix_mode_map); i++) {
      if (strcasecmp(text, mix_mode_map[i].name) == 0) {
        *out_mode = mix_mode_map[i].mode;
        return 0;
      }
    }

  return -1;
}

static int slopay_parse_stereo_mode(const char *text, slopay_stereo_mode_t *out_mode)
{
  static const struct {
    const char           *name;
    slopay_stereo_mode_t  mode;
  } stereo_mode_map[] = {
    { "mono", SLOPAY_STEREO_MODE_MONO },
    { "abc",  SLOPAY_STEREO_MODE_ABC  },
    { "acb",  SLOPAY_STEREO_MODE_ACB  }
  };

  if (text)
    for (size_t i = 0; i < NELEMS(stereo_mode_map); i++) {
      if (strcasecmp(text, stereo_mode_map[i].name) == 0) {
        *out_mode = stereo_mode_map[i].mode;
        return 0;
      }
    }

  return -1;
}

static int slopay_parse_machine(const char *text, slopay_machine_t *out_machine)
{
  static const struct {
    const char      *name;
    slopay_machine_t machine;
  } machine_map[] = {
    { "spectrum", SLOPAY_MACHINE_SPECTRUM },
    { "zx",       SLOPAY_MACHINE_SPECTRUM },
    { "cpc",      SLOPAY_MACHINE_CPC      },
    { "amstrad",  SLOPAY_MACHINE_CPC      }
  };

  if (text)
    for (size_t i = 0; i < NELEMS(machine_map); i++) {
      if (strcasecmp(text, machine_map[i].name) == 0) {
        *out_machine = machine_map[i].machine;
        return 0;
      }
    }

  return -1;
}

static void slopay_sigint_handler(int signum)
{
  (void)signum;
  slopay_stop_requested = 1;
}

static void slopay_inject_interrupt(slopay_z80_t *cpu);

static void slopay_sleep_frame(void)
{
  struct timespec req;

  req.tv_sec = 0;
  req.tv_nsec = 20 * 1000 * 1000; /* 20ms for 50Hz frame rate */
  nanosleep(&req, NULL);
}

static size_t rel_ptr(size_t base, int16_t off)
{
  return (size_t)((int32_t)base + (int32_t)off);
}

static int slopay_is_register_port(uint16_t port)
{
  return (port & 0xC002u) == 0xC000u;
}

static int slopay_is_data_port(uint16_t port)
{
  return (port & 0xC002u) == 0x8000u;
}

static int slopay_is_cpc_ppi_port_a(uint16_t port)
{
  return (port & 0xFF00u) == 0xF400u;
}

static int slopay_is_cpc_ppi_port_c(uint16_t port)
{
  return (port & 0xFF00u) == 0xF600u;
}

static uint8_t slopay_port_read_stub(void *ctx, uint16_t port)
{
  const slopay_io_t *io = ctx;

  if (io != NULL && io->ay != NULL && io->machine == SLOPAY_MACHINE_CPC) {
    if (slopay_is_cpc_ppi_port_a(port) && (io->cpc_psg_control & 0xC0u) == 0x40u)
      return slopay_chip_read_register(io->ay, (slopay_chip_reg_t)(io->selected_reg & 0x0F));
    return 0xFF;
  }

  if (io != NULL && io->ay != NULL && slopay_is_register_port(port))
    return slopay_chip_read_register(io->ay, (slopay_chip_reg_t)(io->selected_reg & 0x0F));

  return 0xFF;
}

static void slopay_port_write_stub(void *ctx, uint16_t port, uint8_t val)
{
  slopay_io_t *io = ctx;

  if (io == NULL)
    return;

  io->total_out_count++;

  if (io->machine == SLOPAY_MACHINE_CPC) {
    if (slopay_is_cpc_ppi_port_c(port)) {
      io->cpc_psg_control = (uint8_t)(val & 0xC0u);

      if (io->cpc_psg_control == 0xC0u) {
        io->selected_reg = (uint8_t)(io->cpc_ppi_port_a & 0x0Fu);
        io->ay_select_count++;
      } else if (io->cpc_psg_control == 0x80u) {
        if (io->ay != NULL)
          slopay_chip_write_register(io->ay,
                                     (slopay_chip_reg_t)(io->selected_reg & 0x0Fu),
                                     io->cpc_ppi_port_a);
        io->ay_write_count++;
      }

      io->other_out_count++;
      return;
    }

    if (slopay_is_cpc_ppi_port_a(port)) {
      io->cpc_ppi_port_a = val;
      io->other_out_count++;
      return;
    }

    io->other_out_count++;
    return;
  }

  if (slopay_is_register_port(port)) {
    io->selected_reg = (uint8_t)(val & 0x0F);
    io->ay_select_count++;
    return;
  }

  if (slopay_is_data_port(port)) {
    if (io->ay != NULL)
      slopay_chip_write_register(io->ay, (slopay_chip_reg_t)(io->selected_reg & 0x0F), val);
    io->ay_write_count++;
    return;
  }

  if ((port & 0x0001u) == 0)
  {
    const int new_level = (val & 0x10u) ? 1 : 0; /* EAR bit */
    if (new_level != io->beeper_level)
      io->beeper_toggle_count++;
    io->beeper_level = new_level;
    io->beeper_out_count++;
  } else
    io->other_out_count++;
}

static uint32_t slopay_preview_samples(slopay_chip_t *ay, int sample_count)
{
  uint32_t hash = 0;
  int i;

  if (ay == NULL)
    return 0;

  for (i = 0; i < sample_count; i++)
    hash = (hash * 16777619u) ^ slopay_chip_get_sample(ay);

  return hash;
}

static int slopay_freq_to_midi(double freq_hz)
{
  int midi;

  if (freq_hz <= 0.0)
    return -1;

  midi = (int)lround(69.0 + 12.0 * log2(freq_hz / 440.0));
  if (midi < 0 || midi > 127)
    return -1;

  return midi;
}

static void slopay_midi_to_note_name(int midi, char *out, size_t out_size)
{
  static const char *names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
  };

  if (midi < 0 || midi > 127) {
    snprintf(out, out_size, "---");
    return;
  }

  snprintf(out, out_size, "%s%d", names[midi % 12], (midi / 12) - 1);
}

static void slopay_ay_channel_note(slopay_chip_t *ay,
                                   int ay_clock_freq,
                                   int ch,
                                   char *out,
                                   size_t out_size)
{
  const uint8_t mixer = slopay_chip_read_register(ay, AY_REG_MIXER);
  const uint8_t vol_reg = slopay_chip_read_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_VOLUME + ch));
  const int tone_enabled = (mixer & (AY_MIXER_NO_TONE_A << ch)) == 0;
  const int noise_enabled = (mixer & (AY_MIXER_NO_NOISE_A << ch)) == 0;
  const int has_level = ((vol_reg & 0x10u) != 0) || ((vol_reg & 0x0Fu) > 0);

  if (!has_level || (!tone_enabled && !noise_enabled)) {
    snprintf(out, out_size, "---");
    return;
  }

  if (!tone_enabled && noise_enabled) {
    snprintf(out, out_size, "NOISE");
    return;
  }

  {
    const uint8_t fine = slopay_chip_read_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_FINE_PITCH + ch * 2));
    const uint8_t coarse = slopay_chip_read_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_COARSE_PITCH + ch * 2));
    const int period = fine | ((coarse & 0x0Fu) << 8);
    const double freq = (period > 0) ? ((double)ay_clock_freq / (16.0 * (double)period)) : 0.0;
    slopay_midi_to_note_name(slopay_freq_to_midi(freq), out, out_size);
  }
}

static int slopay_ay_channel_midi_note(slopay_chip_t *ay, int ay_clock_freq, int ch)
{
  const uint8_t mixer = slopay_chip_read_register(ay, AY_REG_MIXER);
  const uint8_t vol_reg = slopay_chip_read_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_VOLUME + ch));
  const int tone_enabled = (mixer & (AY_MIXER_NO_TONE_A << ch)) == 0;
  const int noise_enabled = (mixer & (AY_MIXER_NO_NOISE_A << ch)) == 0;
  const int has_level = ((vol_reg & 0x10u) != 0) || ((vol_reg & 0x0Fu) > 0);

  if (!has_level || (!tone_enabled && !noise_enabled))
    return -1;

  /* Noise-only output is not mapped to pitched MIDI notes. */
  if (!tone_enabled && noise_enabled)
    return -1;

  {
    const uint8_t fine = slopay_chip_read_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_FINE_PITCH + ch * 2));
    const uint8_t coarse = slopay_chip_read_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_COARSE_PITCH + ch * 2));
    const int period = fine | ((coarse & 0x0Fu) << 8);
    if (period <= 0)
      return -1;
    return slopay_freq_to_midi((double)ay_clock_freq / (16.0 * (double)period));
  }
}

static void slopay_capture_midi_frame(slopay_io_t *io)
{
  uint32_t delta_ticks;

  if (!io->midi_export_enabled)
    return;

  delta_ticks = io->midi_ticks_since_event;
  for (int ch = 0; ch < SLOPAY_MIDI_AY_CHANNELS; ch++) {
    const int new_note = slopay_ay_channel_midi_note(io->ay, io->ay_clock_freq, ch);
    const int old_note = io->midi_notes[ch];

    if (new_note == old_note)
      continue;

    if (old_note >= 0) {
      if (slopay_target_midi_note_off(&io->midi_driver,
                                      delta_ticks,
                                      (uint8_t)ch,
                                      (uint8_t)old_note,
                                      0) != 0) {
        fprintf(stderr, "Warning: MIDI export write failed; disabling MIDI capture\n");
        io->midi_export_enabled = 0;
        return;
      }
      delta_ticks = 0;
    }

    if (new_note >= 0) {
      if (slopay_target_midi_note_on(&io->midi_driver,
                                     delta_ticks,
                                     (uint8_t)ch,
                                     (uint8_t)new_note,
                                     SLOPAY_MIDI_VELOCITY) != 0) {
        fprintf(stderr, "Warning: MIDI export write failed; disabling MIDI capture\n");
        io->midi_export_enabled = 0;
        return;
      }
      delta_ticks = 0;
    }

    io->midi_notes[ch] = new_note;
  }

  {
    const unsigned toggles = io->beeper_toggle_count - io->midi_beeper_toggle_count_last;
    const double beeper_hz = (toggles > 0) ? ((double)toggles * io->frame_rate * 0.5) : 0.0;
    const int new_note = slopay_freq_to_midi(beeper_hz);
    const int old_note = io->midi_notes[SLOPAY_MIDI_BEEPER_VOICE_INDEX];

    io->midi_beeper_toggle_count_last = io->beeper_toggle_count;

    if (new_note != old_note) {
      if (old_note >= 0) {
        if (slopay_target_midi_note_off(&io->midi_driver,
                                        delta_ticks,
                                        io->midi_beeper_channel,
                                        (uint8_t)old_note,
                                        0) != 0) {
          fprintf(stderr, "Warning: MIDI export write failed; disabling MIDI capture\n");
          io->midi_export_enabled = 0;
          return;
        }
        delta_ticks = 0;
      }

      if (new_note >= 0) {
        if (slopay_target_midi_note_on(&io->midi_driver,
                                       delta_ticks,
                                       io->midi_beeper_channel,
                                       (uint8_t)new_note,
                                       SLOPAY_MIDI_VELOCITY) != 0) {
          fprintf(stderr, "Warning: MIDI export write failed; disabling MIDI capture\n");
          io->midi_export_enabled = 0;
          return;
        }
        delta_ticks = 0;
      }

      io->midi_notes[SLOPAY_MIDI_BEEPER_VOICE_INDEX] = new_note;
    }
  }

  io->midi_ticks_since_event = delta_ticks + SLOPAY_MIDI_TICKS_PER_FRAME;
}

static void slopay_finalize_midi_export(slopay_io_t *io)
{
  uint32_t delta_ticks;

  if (!io->midi_export_enabled)
    return;

  delta_ticks = io->midi_ticks_since_event;
  for (int ch = 0; ch < SLOPAY_MIDI_CHANNELS; ch++) {
    if (io->midi_notes[ch] >= 0) {
      if (slopay_target_midi_note_off(&io->midi_driver,
                                      delta_ticks,
                                      (uint8_t)ch,
                                      (uint8_t)io->midi_notes[ch],
                                      0) != 0) {
        fprintf(stderr, "Warning: MIDI export write failed during finalize\n");
        break;
      }
      io->midi_notes[ch] = -1;
      delta_ticks = 0;
    }
  }

  if (slopay_target_midi_cleanup(&io->midi_driver, delta_ticks) != 0)
    fprintf(stderr, "Warning: Failed to finalize MIDI file\n");

  io->midi_export_enabled = 0;
}

static void slopay_emit_piano_roll_frame(slopay_io_t *io)
{
  char note_a[8];
  char note_b[8];
  char note_c[8];
  char note_beeper[8];
  const unsigned toggles = io->beeper_toggle_count - io->beeper_toggle_count_last;
  const double beeper_hz = (toggles > 0) ? ((double)toggles * io->frame_rate * 0.5) : 0.0;

  if (!io->piano_roll_enabled)
    return;

  slopay_ay_channel_note(io->ay, io->ay_clock_freq, 0, note_a, sizeof(note_a));
  slopay_ay_channel_note(io->ay, io->ay_clock_freq, 1, note_b, sizeof(note_b));
  slopay_ay_channel_note(io->ay, io->ay_clock_freq, 2, note_c, sizeof(note_c));
  slopay_midi_to_note_name(slopay_freq_to_midi(beeper_hz), note_beeper, sizeof(note_beeper));
  if (toggles == 0)
    snprintf(note_beeper, sizeof(note_beeper), "---");

  printf("[PR %06d] A=%-6s B=%-6s C=%-6s BEEP=%-6s\n",
         io->played_frames,
         note_a,
         note_b,
         note_c,
         note_beeper);

  io->beeper_toggle_count_last = io->beeper_toggle_count;
}

static void render_audio(void *userdata, float *output, uint32_t frames)
{
  slopay_io_t *io = userdata;

  if (io == NULL || io->ay == NULL || output == NULL)
    return;

  for (uint32_t i = 0; i < frames; i++) {
    float out_l;
    float out_r;

    if (io->played_frames < io->target_frames) {
      int cycles_to_run;

      io->z80_cycle_error_fxp += io->z80_cycles_per_sample_fxp;
      cycles_to_run = io->z80_cycle_error_fxp >> Z80_CYCLE_FXP;
      io->z80_cycle_error_fxp -= (cycles_to_run << Z80_CYCLE_FXP);
      if (cycles_to_run > 0)
        slopay_z80_execute(io->cpu, cycles_to_run);

      if (--io->samples_to_next_frame <= 0) {
        slopay_emit_piano_roll_frame(io);
        slopay_capture_midi_frame(io);
        slopay_inject_interrupt(io->cpu);
        io->played_frames++;
        io->samples_to_next_frame += io->samples_per_frame;
      }
    }

    if (io->played_frames >= io->target_frames) {
      output[i * 2 + 0] = 0.0f;
      output[i * 2 + 1] = 0.0f;
      continue;
    }

    const slopay_chip_sample_t pair = slopay_chip_get_sample(io->ay);
    const int16_t left = (int16_t)(pair & 0xFFFF);
    const int16_t right = (int16_t)(pair >> 16);

    out_l = left / 32767.0f;
    out_r = right / 32767.0f;

    /* Mix in ZX beeper level (captured from EAR bit on even-port OUT). */
    {
      const float beeper = io->beeper_level ? io->beeper_gain : 0.0f;
      const float ay_gain = (io->beeper_mix_mode == SLOPAY_BEEPER_MIX_DUCK && io->beeper_level)
                            ? SLOPAY_BEEPER_DUCK_AY_GAIN
                            : SLOPAY_BEEPER_ADD_AY_GAIN;
      out_l = slopay_clamp_unit(out_l * ay_gain + beeper);
      out_r = slopay_clamp_unit(out_r * ay_gain + beeper);
    }

    output[i * 2 + 0] = out_l;
    output[i * 2 + 1] = out_r;
  }
}

static void slopay_dump_registers(slopay_chip_t *ay)
{
  int reg;

  if (ay == NULL)
    return;

  printf("AY Registers:\n");
  for (reg = 0; reg < 14; reg++) {
    printf("  R%-2d=0x%02X", reg, slopay_chip_read_register(ay, (slopay_chip_reg_t)reg));
    if ((reg & 3) == 3 || reg == 13)
      printf("\n");
    else
      printf("  ");
  }
}

static void slopay_dump_missing_opcodes(const slopay_z80_missing_opcode_stats_t *stats)
{
  static const char *names[3] = { "DD", "FD", "ED" };
  const uint32_t *tables[3] = { stats->dd_counts, stats->fd_counts, stats->ed_counts };

  for (int p = 0; p < 3; p++) {
    uint32_t total = 0;
    int best_op = -1;
    uint32_t best_count = 0;

    for (int op = 0; op < 256; op++) {
      total += tables[p][op];
      if (tables[p][op] > best_count) {
        best_count = tables[p][op];
        best_op = op;
      }
    }

    if (total > 0)
      printf("Missing %s-prefixed opcodes: total=%u top=%02X (%u hits)\n",
             names[p], total, best_op, best_count);
  }
}

/* Inject a maskable interrupt so HALT-based players continue to run. */
static void slopay_inject_interrupt(slopay_z80_t *cpu)
{
  uint16_t vector_addr;
  uint16_t handler_addr;

  if (cpu == NULL)
    return;

  if (!cpu->regs.halted && !cpu->regs.iff1)
    return;

  cpu->regs.halted = 0;

  if (!cpu->regs.iff1)
    return;

  cpu->regs.iff1 = 0;
  cpu->regs.iff2 = 0;

  cpu->regs.sp -= 2;
  cpu->mem[cpu->regs.sp]     = (uint8_t)(cpu->regs.pc & 0xFF);
  cpu->mem[cpu->regs.sp + 1] = (uint8_t)(cpu->regs.pc >> 8);

  switch (cpu->regs.im) {
  case 0:
  case 1:
    cpu->regs.pc = AY_ISR_ADDR;
    break;
  case 2:
    vector_addr = ((uint16_t)cpu->regs.i << 8) | 0x00FF;
    handler_addr = (uint16_t)cpu->mem[vector_addr] |
                   ((uint16_t)cpu->mem[(uint16_t)(vector_addr + 1)] << 8);
    cpu->regs.pc = handler_addr;
    break;
  default:
    cpu->regs.pc = AY_ISR_ADDR;
    break;
  }
}

static void slopay_build_player_v3_memory(slopay_loader_song_t *song)
{
  uint8_t *m = song->z80_memory;
  uint16_t init = song->pointers.init;
  const uint16_t intr = song->pointers.interrupt;

  memset(&m[0x0000], 0xC9, 0x0100);
  memset(&m[0x0100], 0xFF, 0x3F00);
  memset(&m[0x4000], 0x00, 0xC000);
  m[AY_ISR_ADDR] = 0xFB;

  if (init == 0 && song->blocks && song->blocks[0].address != 0)
    init = song->blocks[0].address;

  m[0x0000] = 0xF3; /* DI */
  m[0x0001] = 0xCD; /* CALL INIT */
  m[0x0002] = (uint8_t)(init & 0xFF);
  m[0x0003] = (uint8_t)(init >> 8);

  if (intr == 0) {
    m[0x0004] = 0xED; m[0x0005] = 0x5E; /* IM 2 */
    m[0x0006] = 0xFB;                    /* EI */
    m[0x0007] = 0x76;                    /* HALT */
    m[0x0008] = 0x18; m[0x0009] = (uint8_t)-6; /* JR LOOP */
  } else {
    m[0x0004] = 0xED; m[0x0005] = 0x56; /* IM 1 */
    m[0x0006] = 0xFB;
    m[0x0007] = 0x76;
    m[0x0008] = 0xCD;                    /* CALL INTERRUPT */
    m[0x0009] = (uint8_t)(intr & 0xFF);
    m[0x000A] = (uint8_t)(intr >> 8);
    m[0x000B] = 0x18; m[0x000C] = (uint8_t)-9;
  }
}

static void slopay_load_song_blocks(const slopay_loader_file_t *file, slopay_loader_song_t *song)
{
  int i;

  for (i = 0; song->blocks && song->blocks[i].address != 0; i++) {
    const uint32_t addr = song->blocks[i].address;
    uint32_t len = song->blocks[i].length;
    const size_t src = rel_ptr(song->blocks[i]._offset_base, song->blocks[i].offset);

    if (addr == 0)
      continue;
    if (addr + len > 65536u)
      len = 65536u - addr;
    if (src >= file->file_size)
      continue;
    if (src + len > file->file_size)
      len = (uint32_t)(file->file_size - src);

    memcpy(&song->z80_memory[addr], &file->file_data[src], len);
  }
}

static void slopay_run_z80(slopay_loader_file_t *file,
                           slopay_loader_song_t *song,
                           int volume_percent,
                           int beeper_volume_percent,
                           slopay_beeper_mix_mode_t beeper_mix_mode,
                           slopay_stereo_mode_t stereo_mode,
                           slopay_machine_t machine,
                           int cpc_rate_override,
                           int piano_roll_enabled,
                           int midi_beeper_channel,
                           int max_seconds,
                           int sample_rate,
                           const char *wav_filename,
                           const char *midi_filename)
{
  const slopay_machine_profile_t *profile;
  int effective_interrupt_rate;
  slopay_z80_t *cpu;
  slopay_io_t io;
  int frame_count;
  int frame;
  int audio_started = 0;

  profile = slopay_machine_profile(machine);
  effective_interrupt_rate = profile->interrupt_rate;
  if (machine == SLOPAY_MACHINE_CPC && cpc_rate_override > 0)
    effective_interrupt_rate = cpc_rate_override;

  slopay_build_player_v3_memory(song);
  slopay_load_song_blocks(file, song);

  cpu = slopay_z80_create(song->z80_memory);
  if (cpu == NULL)
    return;

  memset(&io, 0, sizeof(io));
  slopay_z80_missing_opcode_reset();
  io.cpu = cpu;
  io.ay = slopay_chip_create(profile->ay_clock_freq, sample_rate);
  if (io.ay == NULL) {
    slopay_z80_destroy(cpu);
    return;
  }
  slopay_chip_set_volume(io.ay, volume_percent);
  slopay_chip_set_stereo_mode(io.ay,
                              stereo_mode == SLOPAY_STEREO_MODE_MONO
                                ? SLOPAY_CHIP_STEREO_MODE_MONO
                                : (stereo_mode == SLOPAY_STEREO_MODE_ACB
                                    ? SLOPAY_CHIP_STEREO_MODE_ACB
                                    : SLOPAY_CHIP_STEREO_MODE_ABC));

  slopay_z80_set_port_callbacks(cpu, slopay_port_read_stub, slopay_port_write_stub, &io);

  cpu->regs.af = ((uint16_t)song->song_data.hi_reg << 8) | song->song_data.lo_reg;
  cpu->regs.bc = cpu->regs.de = cpu->regs.hl = cpu->regs.af;
  cpu->regs.af_ = cpu->regs.bc_ = cpu->regs.de_ = cpu->regs.hl_ = cpu->regs.af;
  cpu->regs.ix = cpu->regs.iy = cpu->regs.af;
  cpu->regs.i = 3;
  cpu->regs.sp = song->pointers.stack;
  cpu->regs.pc = AY_BOOT_ADDR;
  cpu->regs.iff1 = 0;
  cpu->regs.iff2 = 0;
  cpu->regs.im = 0;

  /* Compute frame count before driver init so WAV mode can validate it. */
  frame_count = song->song_data.song_length > 0
              ? (song->song_data.song_length * effective_interrupt_rate) / 50
              : 0;

  /* Apply -t cap if given, or use song length, or play indefinitely. */
  if (max_seconds > 0) {
    const int cap = max_seconds * effective_interrupt_rate;
    frame_count = (frame_count > 0 && frame_count < cap) ? frame_count : cap;
  } else if (frame_count == 0) {
    frame_count = INT_MAX; /* infinite — play until Ctrl+C */
  }

  /* WAV output requires a known finite duration. */
  if (wav_filename != NULL && frame_count == INT_MAX) {
    fprintf(stderr, "Error: WAV output requires a finite duration.\n"
                    "       The song has no length field; use -t <seconds> to set one.\n");
    slopay_chip_destroy(io.ay);
    slopay_z80_destroy(cpu);
    return;
  }

  /* MIDI output also requires a known finite duration. */
  if (midi_filename != NULL && frame_count == INT_MAX) {
    fprintf(stderr, "Error: MIDI output requires a finite duration.\n"
                    "       The song has no length field; use -t <seconds> to set one.\n");
    slopay_chip_destroy(io.ay);
    slopay_z80_destroy(cpu);
    return;
  }

  io.target_frames = frame_count;
  io.played_frames = 0;
  io.ay_clock_freq = profile->ay_clock_freq;
  io.frame_rate = effective_interrupt_rate;
  io.samples_per_frame = sample_rate / effective_interrupt_rate;
  io.samples_to_next_frame = io.samples_per_frame;
  io.z80_cycles_per_sample_fxp = (profile->z80_clock_freq << Z80_CYCLE_FXP) / sample_rate;
  io.z80_cycle_error_fxp = 0;
  io.beeper_gain = (float)beeper_volume_percent / 100.0f;
  io.beeper_mix_mode = beeper_mix_mode;
  io.piano_roll_enabled = piano_roll_enabled;
  io.midi_export_enabled = 0;
  io.midi_beeper_channel = (uint8_t)midi_beeper_channel;
  io.machine = machine;
  io.cpc_psg_control = 0;
  io.cpc_ppi_port_a = 0;
  io.midi_ticks_since_event = 0;
  for (int ch = 0; ch < SLOPAY_MIDI_CHANNELS; ch++)
    io.midi_notes[ch] = -1;
  io.midi_beeper_toggle_count_last = 0;
  io.beeper_toggle_count_last = 0;

  if (midi_filename != NULL) {
    if (slopay_target_midi_init(&io.midi_driver, midi_filename) != 0) {
      fprintf(stderr, "Error: Failed to open MIDI file '%s'\n", midi_filename);
      slopay_chip_destroy(io.ay);
      slopay_z80_destroy(cpu);
      return;
    }
     io.midi_export_enabled = 1;
     printf("Writing MIDI: %s  (%d frames, %.1f s at %d Hz, beeper channel=%u, machine=%s)\n",
            midi_filename, frame_count,
            (double)frame_count / effective_interrupt_rate, sample_rate,
            io.midi_beeper_channel, profile->name);
   }

  if (wav_filename != NULL) {
    /* ---- WAV file output mode ---------------------------------------- */
    slopay_target_wave_t wav_driver;
    const uint32_t total_samples = (uint32_t)frame_count * (uint32_t)io.samples_per_frame;

     if (slopay_target_wave_init(&wav_driver, wav_filename, sample_rate,
                              render_audio, &io) != 0) {
       fprintf(stderr, "Error: Failed to open WAV file '%s'\n", wav_filename);
      slopay_chip_destroy(io.ay);
      slopay_z80_destroy(cpu);
      return;
    }

     printf("Writing WAV: %s  (%d frames, %.1f s at %d Hz)\n",
            wav_filename, frame_count,
            (double)frame_count / effective_interrupt_rate, sample_rate);

    if (slopay_target_wave_render_all(&wav_driver, total_samples) < 0)
      fprintf(stderr, "Warning: WAV render incomplete\n");

    slopay_target_wave_cleanup(&wav_driver);
    printf("WAV written: %s\n", wav_filename);

   } else {
     /* ---- macOS Core Audio real-time output mode ----------------------- */
     if (slopay_target_macos_init(&io.audio_driver, sample_rate, render_audio, &io) != noErr) {
       fprintf(stderr, "Error: Failed to initialize macOS audio driver\n");
      slopay_finalize_midi_export(&io);
      slopay_chip_destroy(io.ay);
      slopay_z80_destroy(cpu);
      return;
    }

    if (slopay_target_macos_start(&io.audio_driver) != noErr) {
      fprintf(stderr, "Error: Failed to start macOS audio driver\n");
      slopay_target_macos_cleanup(&io.audio_driver);
      slopay_finalize_midi_export(&io);
      slopay_chip_destroy(io.ay);
      slopay_z80_destroy(cpu);
      return;
    }
    audio_started = 1;

    /* Emulation runs in the audio callback; this loop only waits. */
    slopay_stop_requested = 0;
    signal(SIGINT, slopay_sigint_handler);

    for (frame = 0; frame < frame_count && !slopay_stop_requested; frame++)
      slopay_sleep_frame();

    signal(SIGINT, SIG_DFL);

    slopay_target_macos_stop(&io.audio_driver);
    audio_started = 0;
  }

  slopay_finalize_midi_export(&io);
  if (midi_filename != NULL)
    printf("MIDI written: %s\n", midi_filename);

  printf("\nZ80 run complete: PC=0x%04X SP=0x%04X cycles=%u halted=%d OUTs=%u\n",
         cpu->regs.pc, cpu->regs.sp, cpu->cycles, cpu->regs.halted, io.total_out_count);
  printf("AY/Beeper activity: selects=%u writes=%u beeper=%u other=%u\n",
         io.ay_select_count, io.ay_write_count, io.beeper_out_count, io.other_out_count);
  if (io.beeper_out_count > 0)
    printf("Beeper toggles: %u\n", io.beeper_toggle_count);
  if (io.ay_write_count > 0) {
    slopay_dump_registers(io.ay);
    printf("Preview sample hash: 0x%08X\n", slopay_preview_samples(io.ay, 2048));
  }

  slopay_z80_missing_opcode_stats_t missing_stats;
  slopay_z80_missing_opcode_snapshot(&missing_stats);
  slopay_dump_missing_opcodes(&missing_stats);

  if (wav_filename == NULL) {
    if (audio_started)
      slopay_target_macos_stop(&io.audio_driver);
    slopay_target_macos_cleanup(&io.audio_driver);
  }
  slopay_chip_destroy(io.ay);
  slopay_z80_destroy(cpu);
}

static void print_usage(const char *prog)
{
  printf("Usage: %s [-V] [-v <percent>] [-b <percent>] [-m <mode>] [-x <mode>] [-P <machine>] [-I <50|300>] [-r <Hz>] [-p] [-s <song>] [-t <seconds>] [-w <file.wav>] [-M <file.mid>] [-B <channel>] <ay_file>\n", prog);
  printf("\n");
  printf("Loads and displays information about an AY music file.\n");
  printf("\n");
  printf("Options:\n");
  printf("  -v, --volume <percent>          AY volume percent (0-100, default 100)\n");
  printf("  -b, --beeper-volume <percent>   ZX beeper volume percent (0-100, default 22)\n");
  printf("      --beeper <percent>          Alias for --beeper-volume\n");
  printf("  -m, --beeper-mix <mode>         Beeper mix mode: add or duck (default add)\n");
  printf("      --mix <mode>                Alias for --beeper-mix\n");
  printf("  -x, --stereo-mode <mode>        Stereo mode: mono, abc or acb (default abc)\n");
  printf("  -P, --machine <type>            Timing profile: spectrum or cpc (default spectrum)\n");
  printf("  -I, --cpc-rate <50|300>         CPC interrupt rate override (default 50 in cpc mode)\n");
  printf("  -r, --sample-rate <Hz>          Audio sample rate in Hz (8000-192000, default 44100)\n");
  printf("  -p, --piano-roll                Print per-frame AY/Beeper notes during playback\n");
  printf("  -s, --song <song>               Song number to play (0-based)\n");
  printf("                                  (default: play songs sequentially from first song)\n");
  printf("  -t, --time <seconds>            Maximum playback time in seconds\n");
  printf("                                  (0 = use song length; default: song length or Ctrl+C)\n");
  printf("  -w, --wav <file.wav>            Write output to WAV instead of playing through speakers\n");
  printf("                                  (requires a finite duration: song length or -t/--time)\n");
  printf("                                  (sequential mode writes per-song files: <name>-sN.ext)\n");
  printf("  -M, --midi <file.mid>           Export AY + beeper notes to MIDI (format 0)\n");
  printf("                                  (requires a finite duration: song length or -t/--time)\n");
  printf("                                  (sequential mode writes per-song files: <name>-sN.ext)\n");
  printf("  -B, --midi-beeper-channel <0-15> MIDI channel for beeper notes (default 3)\n");
  printf("  -V, --version                   Show program version\n");
  printf("  -h, --help                      Show this help\n");
  printf("\n");
  printf("Arguments:\n");
  printf("  ay_file        Path to the .ay music file\n");
}

static void print_file_info(const slopay_loader_file_t *file)
{
  printf("\n========== AY File Information ==========\n\n");

  printf("File Header:\n");
  printf("  File ID:        %.4s\n", file->header.file_id);
  printf("  Type ID:        %.4s\n", file->header.type_id);
  printf("  File Version:   %d\n", file->header.file_version);
  printf("  Player Version: %d\n", file->header.player_version);

  printf("\nMetadata:\n");
  if (file->author)
    printf("  Author:         %s\n", file->author);
  else
    printf("  Author:         (not specified)\n");

  if (file->misc_info)
    printf("  Information:    %s\n", file->misc_info);
  else
    printf("  Information:    (not specified)\n");

  printf("\nSongs:\n");
  printf("  Total Songs:    %d\n", file->num_songs);
  printf("  First Song:     %d (to play)\n", file->header.first_song);

  printf("\n");
}

static void print_song_info(slopay_loader_file_t *file,
                             uint8_t song_index,
                             int volume_percent,
                             int beeper_volume_percent,
                             slopay_beeper_mix_mode_t beeper_mix_mode,
                             slopay_stereo_mode_t stereo_mode,
                             slopay_machine_t machine,
                             int cpc_rate_override,
                             int piano_roll_enabled,
                             int midi_beeper_channel,
                             int max_seconds,
                             int sample_rate,
                             const char *wav_filename,
                             const char *midi_filename)
{
  const slopay_machine_profile_t *profile = slopay_machine_profile(machine);
  const int effective_interrupt_rate =
      (machine == SLOPAY_MACHINE_CPC && cpc_rate_override > 0)
          ? cpc_rate_override
          : profile->interrupt_rate;
  slopay_loader_song_t *song;
  int block_idx;

  if (song_index >= file->num_songs) {
    fprintf(stderr, "Error: Song index %d out of range (0-%d)\n",
            song_index, file->num_songs - 1);
    return;
  }

  song = slopay_loader_load_song(file, song_index);
  if (song == NULL) {
    fprintf(stderr, "Error: Failed to load song %d\n", song_index);
    return;
  }

  printf("\n========== Song %d Details ==========\n\n", song_index);

  printf("Song Information:\n");
  if (song->name)
    printf("  Name:           %s\n", song->name);
  else
    printf("  Name:           (unnamed)\n");

  printf("\nTiming:\n");
  printf("  Length:         %d/50 seconds", song->song_data.song_length);
  if (song->song_data.song_length == 0)
    printf(" (infinite)");
  printf("\n");
  printf("  Fade Duration:  %d/50 seconds\n", song->song_data.fade_length);
  printf("  Interrupt Rate: %d Hz (%s profile)\n", effective_interrupt_rate, profile->name);

  printf("\nChannel Configuration (EMUL):\n");
  printf("  Channel A:      Amiga %d\n", song->song_data.a_chan);
  printf("  Channel B:      Amiga %d\n", song->song_data.b_chan);
  printf("  Channel C:      Amiga %d\n", song->song_data.c_chan);
  printf("  Noise:          Amiga %d\n", song->song_data.noise_chan);

  printf("\nZ80 Registers:\n");
  printf("  HiReg:          0x%02X\n", song->song_data.hi_reg);
  printf("  LoReg:          0x%02X\n", song->song_data.lo_reg);

  printf("\nZ80 Execution Addresses:\n");
  printf("  Stack (SP):     0x%04X\n", song->pointers.stack);
  printf("  INIT:           0x%04X\n", song->pointers.init);
  printf("  INTERRUPT:      0x%04X\n", song->pointers.interrupt);

  printf("\nData Blocks:\n");
  if (song->blocks == NULL || song->blocks[0].address == 0) {
    printf("  (none)\n");
  } else {
    block_idx = 0;
    while (song->blocks[block_idx].address != 0) {
      printf("  Block %d: Address 0x%04X, Length %d bytes\n",
             block_idx,
             song->blocks[block_idx].address,
             song->blocks[block_idx].length);
      block_idx++;
    }
  }

  printf("\n");

  slopay_run_z80(file,
                 song,
                 volume_percent,
                 beeper_volume_percent,
                 beeper_mix_mode,
                 stereo_mode,
                 machine,
                 cpc_rate_override,
                 piano_roll_enabled,
                 midi_beeper_channel,
                 max_seconds,
                 sample_rate,
                 wav_filename,
                 midi_filename);

  slopay_loader_song_destroy(song);
}

static char *slopay_make_song_export_filename(const char *base, uint8_t song_index)
{
  const char *slash;
  const char *dot;
  size_t stem_len;
  const char *ext;
  int needed;
  char *out;

  if (base == NULL)
    return NULL;

  slash = strrchr(base, '/');
  dot = strrchr(base, '.');
  if (dot != NULL && slash != NULL && dot < slash)
    dot = NULL;

  stem_len = dot ? (size_t)(dot - base) : strlen(base);
  ext = dot ? dot : "";

  needed = snprintf(NULL, 0, "%.*s-s%u%s", (int)stem_len, base, (unsigned)song_index, ext);
  if (needed < 0)
    return NULL;

  out = malloc((size_t)needed + 1u);
  if (out == NULL)
    return NULL;

  snprintf(out, (size_t)needed + 1u, "%.*s-s%u%s", (int)stem_len, base, (unsigned)song_index, ext);
  return out;
}

int main(int argc, char *argv[])
{
  slopay_loader_file_t *file;
  const char *ay_file_path = NULL;
  const char *wav_filename = NULL;
  const char *midi_filename = NULL;
  int opt;
  int volume_percent = 100;
  int beeper_volume_percent = 22;
  int sample_rate = SLOPAY_DEFAULT_SAMPLE_RATE;
  slopay_beeper_mix_mode_t beeper_mix_mode = SLOPAY_BEEPER_MIX_ADD;
  slopay_stereo_mode_t stereo_mode = SLOPAY_STEREO_MODE_ABC;
  slopay_machine_t machine = SLOPAY_MACHINE_SPECTRUM;
  int cpc_rate_override = 0;
  int piano_roll_enabled = 0;
  int midi_beeper_channel = 3;
  int max_seconds = 0;
  uint8_t song_index = 0;
  int have_song_index = 0;
  long parsed;
  char *endptr;

  static const struct option long_opts[] = {
    { "help",          no_argument,       NULL, 'h' },
    { "version",       no_argument,       NULL, 'V' },
    { "volume",        required_argument, NULL, 'v' },
    { "beeper-volume", required_argument, NULL, 'b' },
    { "beeper",        required_argument, NULL, 'b' },
    { "beeper-mix",    required_argument, NULL, 'm' },
    { "mix",           required_argument, NULL, 'm' },
    { "stereo-mode",   required_argument, NULL, 'x' },
    { "machine",       required_argument, NULL, 'P' },
    { "cpc-rate",      required_argument, NULL, 'I' },
    { "sample-rate",   required_argument, NULL, 'r' },
    { "piano-roll",    no_argument,       NULL, 'p' },
    { "song",          required_argument, NULL, 's' },
    { "time",          required_argument, NULL, 't' },
    { "wav",           required_argument, NULL, 'w' },
    { "midi",          required_argument, NULL, 'M' },
    { "midi-beeper-channel", required_argument, NULL, 'B' },
    { NULL,             0,                 NULL,  0  }
  };

  while ((opt = getopt_long(argc, argv, "hVv:b:m:x:P:I:r:ps:t:w:M:B:", long_opts, NULL)) != -1) {
    switch (opt) {
    case 'h':
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    case 'V':
      printf("SlopAY %s\n", SLOPAY_VERSION_STRING);
      return EXIT_SUCCESS;
    case 'v':
      parsed = strtol(optarg, &endptr, 10);
      if (*optarg == '\0' || *endptr != '\0' || parsed < 0 || parsed > 100) {
        fprintf(stderr, "Error: Volume must be an integer from 0 to 100\n");
        return EXIT_FAILURE;
      }
      volume_percent = (int)parsed;
      break;
    case 'b':
      parsed = strtol(optarg, &endptr, 10);
      if (*optarg == '\0' || *endptr != '\0' || parsed < 0 || parsed > 100) {
        fprintf(stderr, "Error: Beeper volume must be an integer from 0 to 100\n");
        return EXIT_FAILURE;
      }
      beeper_volume_percent = (int)parsed;
      break;
    case 'm':
      if (slopay_parse_beeper_mix_mode(optarg, &beeper_mix_mode) != 0) {
        fprintf(stderr,
                "Error: Beeper mix must be one of: add, additive, duck, ducking\n");
        return EXIT_FAILURE;
      }
      break;
    case 'x':
      if (slopay_parse_stereo_mode(optarg, &stereo_mode) != 0) {
        fprintf(stderr, "Error: Stereo mode must be one of: mono, abc, acb\n");
        return EXIT_FAILURE;
      }
      break;
    case 'P':
      if (slopay_parse_machine(optarg, &machine) != 0) {
        fprintf(stderr, "Error: Machine must be one of: spectrum, cpc\n");
        return EXIT_FAILURE;
      }
      break;
    case 'I':
      parsed = strtol(optarg, &endptr, 10);
      if (*optarg == '\0' || *endptr != '\0' || (parsed != 50 && parsed != 300)) {
        fprintf(stderr, "Error: CPC rate must be 50 or 300\n");
        return EXIT_FAILURE;
      }
     cpc_rate_override = (int)parsed;
       break;
     case 'r':
       parsed = strtol(optarg, &endptr, 10);
       if (*optarg == '\0' || *endptr != '\0' || parsed < 8000 || parsed > 192000) {
         fprintf(stderr, "Error: Sample rate must be an integer from 8000 to 192000 Hz\n");
         return EXIT_FAILURE;
       }
       sample_rate = (int)parsed;
       break;
     case 'p':
      piano_roll_enabled = 1;
      break;
    case 's':
      parsed = strtol(optarg, &endptr, 10);
      if (*optarg == '\0' || *endptr != '\0' || parsed < 0 || parsed > 255) {
        fprintf(stderr, "Error: Song must be an integer from 0 to 255\n");
        return EXIT_FAILURE;
      }
      song_index = (uint8_t)parsed;
      have_song_index = 1;
      break;
    case 't':
      parsed = strtol(optarg, &endptr, 10);
      if (*optarg == '\0' || *endptr != '\0' || parsed < 0) {
        fprintf(stderr, "Error: Time must be a non-negative integer (seconds)\n");
        return EXIT_FAILURE;
      }
      max_seconds = (int)parsed;
      break;
    case 'w':
      wav_filename = optarg;
      break;
    case 'M':
      midi_filename = optarg;
      break;
    case 'B':
      parsed = strtol(optarg, &endptr, 10);
      if (*optarg == '\0' || *endptr != '\0' || parsed < 0 || parsed > 15) {
        fprintf(stderr, "Error: MIDI beeper channel must be an integer from 0 to 15\n");
        return EXIT_FAILURE;
      }
      midi_beeper_channel = (int)parsed;
      break;
    default:
      fprintf(stderr, "Error: Unknown or malformed option\n");
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (optind >= argc) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  ay_file_path = argv[optind++];

  if (optind < argc) {
    fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[optind]);
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  /* Load AY file */
  printf("Loading AY file: %s\n", ay_file_path);
  printf("AY volume: %d%%\n", volume_percent);
  printf("Beeper volume: %d%%\n", beeper_volume_percent);
  printf("Beeper mix: %s\n", slopay_beeper_mix_mode_name(beeper_mix_mode));
  printf("Stereo mode: %s\n", slopay_stereo_mode_name(stereo_mode));
  printf("Machine profile: %s\n", slopay_machine_name(machine));
  if (machine == SLOPAY_MACHINE_CPC && cpc_rate_override > 0)
    printf("CPC interrupt rate override: %d Hz\n", cpc_rate_override);
  printf("Sample rate: %d Hz\n", sample_rate);
  printf("Piano roll: %s\n", piano_roll_enabled ? "on" : "off");
  if (midi_filename != NULL) {
    printf("MIDI export: %s\n", midi_filename);
    printf("MIDI beeper channel: %d\n", midi_beeper_channel);
  }
  file = slopay_loader_load_file(ay_file_path);
  if (file == NULL) {
    fprintf(stderr, "Error: Failed to load AY file\n");
    return EXIT_FAILURE;
  }

  /* Display file information */
  print_file_info(file);

  /* Display song list */
  printf("Available Songs:\n");
  for (uint8_t i = 0; i < file->num_songs; i++) {
    char *song_name = slopay_loader_get_song_name(file, i);
    printf("  [%d] %s\n", i, song_name ? song_name : "(unnamed)");
    if (song_name)
      free(song_name);
  }

  /* Display details for requested song, or auto-play all songs in order. */
  if (have_song_index) {
    print_song_info(file,
                    song_index,
                    volume_percent,
                    beeper_volume_percent,
                    beeper_mix_mode,
                    stereo_mode,
                    machine,
                    cpc_rate_override,
                    piano_roll_enabled,
                    midi_beeper_channel,
                    max_seconds,
                    sample_rate,
                    wav_filename,
                    midi_filename);
  } else if (file->num_songs > 0) {
    int start = file->header.first_song;
    int count = file->num_songs;

    if (start < 0 || start >= count)
      start = 0;

    for (int n = 0; n < count; n++) {
      const uint8_t idx = (uint8_t)((start + n) % count);
      char *wav_song_filename = NULL;
      char *midi_song_filename = NULL;

      if (wav_filename != NULL)
        wav_song_filename = slopay_make_song_export_filename(wav_filename, idx);
      if (midi_filename != NULL)
        midi_song_filename = slopay_make_song_export_filename(midi_filename, idx);

      print_song_info(file,
                      idx,
                      volume_percent,
                      beeper_volume_percent,
                      beeper_mix_mode,
                      stereo_mode,
                      machine,
                      cpc_rate_override,
                      piano_roll_enabled,
                      midi_beeper_channel,
                      max_seconds,
                      sample_rate,
                      wav_song_filename ? wav_song_filename : wav_filename,
                      midi_song_filename ? midi_song_filename : midi_filename);
      free(wav_song_filename);
      free(midi_song_filename);
      if (slopay_stop_requested)
        break;
    }
  }

  /* Cleanup */
  slopay_loader_file_destroy(file);

  return EXIT_SUCCESS;
}
