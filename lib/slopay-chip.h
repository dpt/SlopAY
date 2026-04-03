/* slopay-chip.h
*
 * AY-3-8912 sound chip emulator
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 */

#ifndef SLOPAY_CHIP_H
#define SLOPAY_CHIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AY constants */
#define AY_CHANNELS                      (3)

/* Register addresses */
#define AY_REG_CHANNEL_A_FINE_PITCH      (0)
#define AY_REG_CHANNEL_A_COARSE_PITCH    (1)
#define AY_REG_CHANNEL_B_FINE_PITCH      (2)
#define AY_REG_CHANNEL_B_COARSE_PITCH    (3)
#define AY_REG_CHANNEL_C_FINE_PITCH      (4)
#define AY_REG_CHANNEL_C_COARSE_PITCH    (5)
#define AY_REG_NOISE_PITCH               (6)
#define AY_REG_MIXER                     (7)
#define AY_REG_CHANNEL_A_VOLUME          (8)
#define AY_REG_CHANNEL_B_VOLUME          (9)
#define AY_REG_CHANNEL_C_VOLUME         (10)
#define AY_REG_ENVELOPE_FINE_DURATION   (11)
#define AY_REG_ENVELOPE_COARSE_DURATION (12)
#define AY_REG_ENVELOPE_SHAPE           (13)

/* Mixer flags (active low). */
#define AY_MIXER_NO_TONE_A            (1 << 0)
#define AY_MIXER_NO_TONE_B            (1 << 1)
#define AY_MIXER_NO_TONE_C            (1 << 2)
#define AY_MIXER_NO_NOISE_A           (1 << 3)
#define AY_MIXER_NO_NOISE_B           (1 << 4)
#define AY_MIXER_NO_NOISE_C           (1 << 5)
#define AY_MIXER_ALL_ON               (0)
#define AY_MIXER_TONE_ON              (7)
#define AY_MIXER_ALL_OFF              (0x3F)

typedef struct slopay_chip slopay_chip_t;
typedef uint8_t slopay_chip_reg_t;
typedef uint32_t slopay_chip_sample_t; /* 0xRRRRLLLL */

slopay_chip_t *slopay_chip_create(int clock_freq, int sample_rate);
void slopay_chip_destroy(slopay_chip_t *chip);

void slopay_chip_write_register(slopay_chip_t *chip, slopay_chip_reg_t reg, uint8_t value);
uint8_t slopay_chip_read_register(slopay_chip_t *chip, slopay_chip_reg_t reg);

slopay_chip_sample_t slopay_chip_get_sample(slopay_chip_t *chip);
void slopay_chip_set_volume(slopay_chip_t *chip, int volume);
void slopay_chip_set_stereo(slopay_chip_t *chip, int stereo);

#ifdef __cplusplus
}
#endif

#endif /* SLOPAY_CHIP_H */

