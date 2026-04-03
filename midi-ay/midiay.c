/* midiay.c
 *
 * Test shell for the AY emulator that runs on macOS and accepts MIDI input.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include "slopay-chip.h"
#include "slopay-target-macos.h"
#include "polyblep.h"
#include "reverb.h"

/* ----------------------------------------------------------------------- */

/* MIDI */
#define MIDI_A4                 (69) /* 440Hz */

/* MIDI Status Bytes */
#define MIDI_NOTE_OFF         (0x80)
#define MIDI_NOTE_ON          (0x90)
#define MIDI_CONTROL_CHANGE   (0xB0)
#define MIDI_PROGRAM_CHANGE   (0xC0)
#define MIDI_CHANNEL_PRESSURE (0xD0)
#define MIDI_PITCH_BEND       (0xE0)

/* MIDI Controllers */
#define MIDI_MOD_WHEEL           (1)
#define MIDI_VOLUME              (7)

/* MIDI Constants */
#define MIDI_MAX_VALUE         (127)

/* ----------------------------------------------------------------------- */

/* AY configuration */
#define SAMPLE_RATE          (44100) /* Standard CD-quality sample rate */
#define CLOCK_FREQ         (1773450) /* ZX Spectrum 128K AY clock frequency in Hz */

#define MAX_POLYPHONY  (AY_CHANNELS)

#define MIDI_DEBUG               (0) /* Set to 1 to print MIDI events to console */

/* ----------------------------------------------------------------------- */

#define DIV_NEAREST(a,b) (((a) + (b) / 2) / (b))

/* ----------------------------------------------------------------------- */

static slopay_chip_t        *ay;
static polyblep_osc_t        oscs[MAX_POLYPHONY];
static reverb_t             *rev[2];
static slopay_target_macos_t audio_driver;
static int                   output_polyblep;

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

static int parse_volume_command(const char *input, int *value_out)
{
  char *endptr;
  long  value;

  if (input[0] != 'V' && input[0] != 'v')
    return 0;

  endptr = (char *)input + 1;
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

static int parse_master_volume_command(const char *input, int *value_out)
{
  char *endptr;
  long  value;

  if (input[0] != 'M' && input[0] != 'm')
    return 0;

  endptr = (char *)input + 1;
  while (*endptr == ' ' || *endptr == '\t')
    endptr++;

  value = strtol(endptr, &endptr, 10);
  if (value < 0 || value > 100)
    return 0;

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n')
    endptr++;
  if (*endptr != '\0')
    return 0;

  *value_out = (int)value;
  return 1;
}

/* ----------------------------------------------------------------------- */

static void render_audio(void *userdata, float *output, uint32_t frames)
{
  int16_t left, right;
  pbflt_t o1, o2, o3;

  (void)userdata;

  for (uint32_t i = 0; i < frames; i++) {
    if (output_polyblep) {
      o1 = polyblep_sample(&oscs[0]);
      o2 = polyblep_sample(&oscs[1]);
      o3 = polyblep_sample(&oscs[2]);
      const float mixed = (o1 + o2 + o3) / 3.0f * 32767.0f * 0.10f;
      left = right = (int16_t)lroundf(mixed);
    } else {
      const slopay_chip_sample_t pair = slopay_chip_get_sample(ay);
      left  = (int16_t)(uint16_t)(pair & 0xFFFFu);
      right = (int16_t)(uint16_t)((pair >> 16) & 0xFFFFu);
    }

    left  = reverb_process(rev[0], left);
    right = reverb_process(rev[1], right);

    output[i * 2 + 0] = left  / 32767.0f; /* Scale to -1.0 to 1.0 */
    output[i * 2 + 1] = right / 32767.0f;
  }
}

/* ----------------------------------------------------------------------- */

static int channel_to_note[MAX_POLYPHONY];
static int mixerstate;
static int envshape;

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
    if (channel_to_note[ch] == 0)
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
  slopay_chip_write_register(ay, AY_REG_CHANNEL_A_FINE_PITCH + ch * 2, ay_period & 0xFF);
  slopay_chip_write_register(ay, AY_REG_CHANNEL_A_COARSE_PITCH + ch * 2, ay_period >> 8);
  slopay_chip_write_register(ay, AY_REG_ENVELOPE_SHAPE, envshape);

  /* slopay_chip_write_register(ay, AY_REG_NOISE_PITCH, note & 31); // 0..31 */

  mixerstate |= AY_MIXER_NO_TONE_A << ch; /* inverted sense here */
  /* mixerstate |= AY_MIXER_NO_NOISE_A << ch; // inverted sense here */
  slopay_chip_write_register(ay, AY_REG_MIXER, ~mixerstate); /* enable */

  polyblep_set_freq(&oscs[ch], freq);
  polyblep_enable(&oscs[ch], 1);

  channel_to_note[ch] = note;
}

static void key_release(int note)
{
  int ch;

  /* Find the channel playing the note we want to stop */
  for (ch = 0; ch < MAX_POLYPHONY; ch++)
    if (channel_to_note[ch] == note)
      break;
  if (ch == MAX_POLYPHONY)
    return; /* not playing that note */

  mixerstate &= ~(AY_MIXER_NO_TONE_A << ch);
  mixerstate &= ~(AY_MIXER_NO_NOISE_A << ch);
  slopay_chip_write_register(ay, AY_REG_MIXER, ~mixerstate);

  polyblep_enable(&oscs[ch], 0);

  channel_to_note[ch] = 0;
}

static void key_release_all(void)
{
  int ch;

  for (ch = 0; ch < MAX_POLYPHONY; ch++) {
    mixerstate &= ~(AY_MIXER_NO_TONE_A << ch);
    mixerstate &= ~(AY_MIXER_NO_NOISE_A << ch);
    slopay_chip_write_register(ay, AY_REG_MIXER, ~mixerstate);

    polyblep_enable(&oscs[ch], 0);

    channel_to_note[ch] = 0;
  }
}

static void setchannelvol(float midi_volume)
{
  int ch;
  const uint8_t ay_volume = (uint8_t)lroundf(midi_volume * 15.0f);

  printf("Setting channel volumes to %.2f\n", midi_volume);
  for (ch = 0; ch < MAX_POLYPHONY; ch++) {
    slopay_chip_write_register(ay, (slopay_chip_reg_t)(AY_REG_CHANNEL_A_VOLUME + ch), ay_volume);
    polyblep_set_pw(&oscs[ch], midi_volume);
  }
}

static void setenvshape(void)
{
  envshape = (envshape + 1) % 16;
  printf("Setting envelope shape to %d\n", envshape);
}

static void setenvperiod(void)
{
  static int period;
  period = (period + 16) % 256;
  printf("Setting envelope period to %d\n", period);
  slopay_chip_write_register(ay, AY_REG_ENVELOPE_FINE_DURATION, 0);
  slopay_chip_write_register(ay, AY_REG_ENVELOPE_COARSE_DURATION, period);
}

static void cyclestereomode(void)
{
  static int mode = SLOPAY_CHIP_STEREO_MODE_MONO;
  const char *name;

  mode = (mode + 1) % 3;
  slopay_chip_set_stereo_mode(ay, (slopay_chip_stereo_mode_t)mode);

  if (mode == SLOPAY_CHIP_STEREO_MODE_ACB)
    name = "acb";
  else if (mode == SLOPAY_CHIP_STEREO_MODE_ABC)
    name = "abc";
  else
    name = "mono";

  printf("Setting stereo mode to %s\n", name);
}

/* ----------------------------------------------------------------------- */

static MIDIClientRef   midiClient = 0;
static MIDIPortRef     inputPort = 0;

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
        key_release(data1);
        break;
      case MIDI_NOTE_ON: /* Note On */
        printf("  Note On: Channel %d, Note %d, Velocity %d\n",
               channel + 1, data1, data2);
        if (data2)
          key_hold(data1);
        else
          key_release(data1);
        break;
      case MIDI_CONTROL_CHANGE: /* Control Change */
        printf("  Control Change: Channel %d, Controller %d, Value %d\n",
               channel + 1, data1, data2);
        if (data1 == MIDI_MOD_WHEEL) { /* mod */
          if (data2 == MIDI_MAX_VALUE)
            output_polyblep = !output_polyblep;
          printf("use_polyblep=%d\n", output_polyblep);
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

static int setupMIDI(void)
{
  const ItemCount sourceCount = MIDIGetNumberOfSources();
  ItemCount i;
  OSStatus status;

  CFStringRef clientName = CFSTR("MIDI Monitor");
  status = MIDIClientCreate(clientName, NULL, NULL, &midiClient);
  if (status != noErr)
    return -1;

  CFStringRef endpointName = CFSTR("MIDI Input");
  status = MIDIInputPortCreate(midiClient, endpointName, midiMessageCallback, NULL, &inputPort);
  if (status != noErr) {
    MIDIClientDispose(midiClient);
    midiClient = 0;
    return -1;
  }

  for (i = 0; i < sourceCount; i++)
    MIDIPortConnectSource(inputPort, MIDIGetSource(i), NULL);

  return 0;
}

static void teardownMIDI(void)
{
  if (inputPort != 0) {
    MIDIPortDispose(inputPort);
    inputPort = 0;
  }
  if (midiClient != 0) {
    MIDIClientDispose(midiClient);
    midiClient = 0;
  }
}

/* ----------------------------------------------------------------------- */

static void repl(void)
{
  char input[100];
  int  reg, val, volume_value, master_volume_value;
  int  cmd;

  printf("Enter register and value (e.g., '0 128' for reg 0, value 128) to program AY\n");
  printf("Enter a note A..G to play (holding), or '.' to stop all notes\n");
  printf("Enter 'v <0-127>' to set channel volume from MIDI scale\n");
  printf("Enter 'm <0-100>' to set master volume percent\n");
  printf("Enter 's' to set envelope shape, 'p' to set envelope period, 'r' to set reverb delay, 't' to cycle stereo mode (mono/abc/acb) or 'q' to quit\n");

  for (;;) {
    printf("Command> ");
    if (!fgets(input, sizeof(input), stdin))
      break;

    cmd = input[0];
    if (cmd >= 'a' && cmd <= 'z')
      cmd -= ('a' - 'A');

    if (cmd == 'Q') {
      break;
    } else if (cmd == 'S') {
      setenvshape();
    } else if (cmd == 'P') {
      setenvperiod();
    } else if (cmd == 'R') {
      static size_t delay = SAMPLE_RATE;
      delay = (delay >> 1);
      if (delay < SAMPLE_RATE / 16)
        delay = SAMPLE_RATE;
      reverb_set_delay(rev[0], delay);
      reverb_set_delay(rev[1], delay);
      printf("Set reverb delay to %zu samples (%.2f ms)\n", delay, (double)delay / SAMPLE_RATE * 1000);
    } else if (cmd == 'T') {
      cyclestereomode();
    } else if (cmd >= 'A' && cmd <= 'G') {
      key_hold(MIDI_A4 + cmd - 'A');
    } else if (cmd == '.') {
      key_release_all();
    } else if (parse_master_volume_command(input, &master_volume_value)) {
      slopay_chip_set_volume(ay, master_volume_value);
      printf("Set master volume to %d%%\n", master_volume_value);
    } else if (parse_volume_command(input, &volume_value)) {
      setchannelvol((float)volume_value / (float)MIDI_MAX_VALUE);
    } else if (parse_register_write(input, &reg, &val)) {
      slopay_chip_write_register(ay, (slopay_chip_reg_t)reg, (uint8_t)val);
      printf("Wrote %d to register %d\n", val, reg);
    } else {
      printf("Invalid input. Use 'q' to quit\n");
    }
  }
}

/* ----------------------------------------------------------------------- */

int main(void)
{
  int ch;

  printf("AY-3-8912 Emulator with CoreAudio\n");
  printf("=================================\n");

  printf("Sample rate: %dHz\n", SAMPLE_RATE);
  printf("Clock speed: %.2fMHz\n", (double)CLOCK_FREQ / 1000000.0);

  /* Initialize AY state */
  ay = slopay_chip_create(CLOCK_FREQ, SAMPLE_RATE);
  if (ay == NULL) {
    printf("Failed to create AY\n");
    goto failure;
  }

  /* Set defaults */
  for (ch = 0; ch < AY_CHANNELS; ch++) {
    slopay_chip_write_register(ay, AY_REG_CHANNEL_A_VOLUME + ch, 15); /* Set channel volume (max) */
    polyblep_init(&oscs[ch], 440, SAMPLE_RATE);  /* 440Hz at 44.1kHz sample rate */
  }
  slopay_chip_write_register(ay, AY_REG_ENVELOPE_SHAPE, 0);
  slopay_chip_write_register(ay, AY_REG_ENVELOPE_FINE_DURATION, 0);
  slopay_chip_write_register(ay, AY_REG_ENVELOPE_COARSE_DURATION, 4);
  slopay_chip_set_stereo_mode(ay, SLOPAY_CHIP_STEREO_MODE_ABC); /* default stereo mode */
  slopay_chip_set_volume(ay, 10); /* 10% master volume */

  rev[0] = reverb_create(SAMPLE_RATE);
  if (rev[0] == NULL) {
    printf("Failed to create reverb\n");
    goto failure;
  }
  rev[1] = reverb_create(SAMPLE_RATE);
  if (rev[1] == NULL) {
    printf("Failed to create reverb\n");
    goto failure;
  }

  if (setupMIDI() != 0) {
    printf("Failed to initialize MIDI\n");
    goto failure;
  }

  /* Initialize CoreAudio */
  OSStatus result = slopay_target_macos_init(&audio_driver, SAMPLE_RATE * 1, render_audio, NULL); /* 1 second max */
  if (result != noErr) {
    printf("Failed to initialize audio\n");
    goto failure;
  }

  /* Start playback */
  result = slopay_target_macos_start(&audio_driver);
  if (result != noErr) {
    printf("Failed to start audio playback\n");
    slopay_target_macos_cleanup(&audio_driver);
    goto failure;
  }

  repl();

  /* Stop and cleanup */
  slopay_target_macos_stop(&audio_driver);
  slopay_target_macos_cleanup(&audio_driver);
  teardownMIDI();
  reverb_destroy(rev[1]);
  reverb_destroy(rev[0]);
  slopay_chip_destroy(ay);

  printf("Test completed\n");

  exit(EXIT_SUCCESS);

failure:
  slopay_target_macos_cleanup(&audio_driver);
  teardownMIDI();
  if (rev[1] != NULL) {
    reverb_destroy(rev[1]);
    rev[1] = NULL;
  }
  if (rev[0] != NULL) {
    reverb_destroy(rev[0]);
    rev[0] = NULL;
  }
  if (ay != NULL) {
    slopay_chip_destroy(ay);
    ay = NULL;
  }
  exit(EXIT_FAILURE);
}


