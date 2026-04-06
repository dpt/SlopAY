/* slopay-target-macos.h
 *
 * macOS audio driver implementation using Core Audio.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SLOPAY_TARGET_MACOS_H
#define SLOPAY_TARGET_MACOS_H

#include <AudioToolbox/AudioToolbox.h>

#include "slopay-render.h"

typedef struct slopay_target_macos {
  AudioUnit        audio_unit;
  slopay_render_fn render;
  void            *userdata;
} slopay_target_macos_t;

OSStatus slopay_target_macos_init(slopay_target_macos_t *driver,
                                   int                   sample_rate,
                                   slopay_render_fn      render,
                                   void                 *userdata);
OSStatus slopay_target_macos_start(const slopay_target_macos_t *driver);
OSStatus slopay_target_macos_stop(const slopay_target_macos_t *driver);
void slopay_target_macos_cleanup(slopay_target_macos_t *driver);

#endif /* SLOPAY_TARGET_MACOS_H */
