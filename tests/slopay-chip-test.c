/* slopay-chip-test.c
 *
 * Basic unit tests for the AY chip core.
 */

#include <assert.h>
#include <stddef.h>

#include "slopay-chip.h"

static void test_register_masking(void)
{
  slopay_chip_t *chip = slopay_chip_create(1773450, 44100);
  assert(chip != NULL);

  slopay_chip_write_register(chip, AY_REG_CHANNEL_A_COARSE_PITCH, 0xFF);
  assert(slopay_chip_read_register(chip, AY_REG_CHANNEL_A_COARSE_PITCH) == 0x0F);

  slopay_chip_write_register(chip, AY_REG_NOISE_PITCH, 0xFF);
  assert(slopay_chip_read_register(chip, AY_REG_NOISE_PITCH) == 0x1F);

  slopay_chip_write_register(chip, AY_REG_MIXER, 0xFF);
  assert(slopay_chip_read_register(chip, AY_REG_MIXER) == 0x3F);

  slopay_chip_write_register(chip, AY_REG_CHANNEL_A_VOLUME, 0xFF);
  assert(slopay_chip_read_register(chip, AY_REG_CHANNEL_A_VOLUME) == 0x1F);

  slopay_chip_write_register(chip, AY_REG_ENVELOPE_SHAPE, 0xFF);
  assert(slopay_chip_read_register(chip, AY_REG_ENVELOPE_SHAPE) == 0x0F);

  slopay_chip_destroy(chip);
}

static void test_volume_only_output_is_audible(void)
{
  slopay_chip_t *chip = slopay_chip_create(1773450, 44100);
  int i;
  int non_zero_seen = 0;

  assert(chip != NULL);

  slopay_chip_set_stereo_mode(chip, SLOPAY_CHIP_STEREO_MODE_MONO);
  slopay_chip_set_volume(chip, 100);
  slopay_chip_write_register(chip, AY_REG_MIXER, AY_MIXER_ALL_OFF);
  slopay_chip_write_register(chip, AY_REG_CHANNEL_A_VOLUME, 0x0F);

  for (i = 0; i < 64; i++) {
    const slopay_chip_sample_t sample = slopay_chip_get_sample(chip);
    if (sample != 0) {
      non_zero_seen = 1;
      break;
    }
  }

  assert(non_zero_seen);
  slopay_chip_destroy(chip);
}

static void test_master_volume_zero_mutes_output(void)
{
  slopay_chip_t *chip = slopay_chip_create(1773450, 44100);
  int i;

  assert(chip != NULL);

  slopay_chip_set_stereo_mode(chip, SLOPAY_CHIP_STEREO_MODE_MONO);
  slopay_chip_write_register(chip, AY_REG_MIXER, AY_MIXER_ALL_OFF);
  slopay_chip_write_register(chip, AY_REG_CHANNEL_A_VOLUME, 0x0F);
  slopay_chip_set_volume(chip, 0);

  for (i = 0; i < 16; i++)
    assert(slopay_chip_get_sample(chip) == 0);

  slopay_chip_destroy(chip);
}

int main(void)
{
  test_register_masking();
  test_volume_only_output_is_audible();
  test_master_volume_zero_mutes_output();
  return 0;
}
