/* slopay-render.h
 *
 * Common audio render callback type for output drivers.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SLOPAY_RENDER_H
#define SLOPAY_RENDER_H

#include <stdint.h>

/**
 * Callback signature for audio frame rendering.
 *
 * \param[in]  userdata Caller-provided context.
 * \param[out] output   Buffer to fill with interleaved float32 samples (-1.0 to +1.0).
 * \param[in]  frames   Number of sample frames to render (mono or stereo).
 */
typedef void (*slopay_render_fn)(void     *userdata,
                                 float    *output,
                                 uint32_t frames);

#endif /* SLOPAY_RENDER_H */

