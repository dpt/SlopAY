/* slopay-target-macos.c
 *
 * macOS audio driver implementation using Core Audio.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#include "slopay-target-macos.h"

#include <stdio.h>

static OSStatus slopay_target_macos_callback(void                       *inRefCon,
                                             AudioUnitRenderActionFlags *ioActionFlags,
                                             const AudioTimeStamp       *inTimeStamp,
                                             UInt32                      inBusNumber,
                                             UInt32                      inNumberFrames,
                                             AudioBufferList            *ioData)
{
  slopay_target_macos_t *driver = inRefCon;
  const AudioBuffer     *buffer = &ioData->mBuffers[0];
  float                 *output = buffer->mData;

  /* CoreAudio callback signature requires a mutable pointer here. */
  (void)ioActionFlags;
  (void)inTimeStamp;
  (void)inBusNumber;

  if (driver != NULL && driver->render != NULL)
    driver->render(driver->userdata, output, inNumberFrames);

  return noErr;
}

OSStatus slopay_target_macos_init(slopay_target_macos_t              *driver,
                                  const int                           sample_rate,
                                  const slopay_target_macos_render_fn render,
                                  void                               *userdata)
{
  OSStatus                    result;
  AudioComponentDescription   desc;
  AudioStreamBasicDescription format;
  AURenderCallbackStruct      render_callback;

  if (driver == NULL || render == NULL)
    return kAudio_ParamError;

  memset(driver, 0, sizeof(*driver));
  driver->render   = render;
  driver->userdata = userdata;

  desc.componentType         = kAudioUnitType_Output;
  desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  desc.componentFlags        = 0;
  desc.componentFlagsMask    = 0;

  AudioComponent component = AudioComponentFindNext(NULL, &desc);
  if (component == NULL) {
    printf("Failed to find audio component\n");
    return kAudio_ParamError;
  }

  result = AudioComponentInstanceNew(component, &driver->audio_unit);
  if (result != noErr) {
    printf("Failed to create audio unit instance\n");
    return result;
  }

  format.mSampleRate       = sample_rate;
  format.mFormatID         = kAudioFormatLinearPCM;
  format.mFormatFlags      = kAudioFormatFlagIsFloat;
  format.mBytesPerPacket   = sizeof(float) * 2;
  format.mFramesPerPacket  = 1;
  format.mBytesPerFrame    = sizeof(float) * 2;
  format.mChannelsPerFrame = 2;
  format.mBitsPerChannel   = sizeof(float) * 8;
  format.mReserved         = 0;

  result = AudioUnitSetProperty(driver->audio_unit,
                                kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Input,
                                0,
                                &format,
                                sizeof(format));
  if (result != noErr) {
    printf("Failed to set stream format\n");
    slopay_target_macos_cleanup(driver);
    return result;
  }

  render_callback.inputProc       = slopay_target_macos_callback;
  render_callback.inputProcRefCon = driver;

  result = AudioUnitSetProperty(driver->audio_unit,
                                kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input,
                                0,
                                &render_callback,
                                sizeof(render_callback));
  if (result != noErr) {
    printf("Failed to set render callback\n");
    slopay_target_macos_cleanup(driver);
    return result;
  }

  result = AudioUnitInitialize(driver->audio_unit);
  if (result != noErr) {
    printf("Failed to initialize audio unit\n");
    slopay_target_macos_cleanup(driver);
    return result;
  }

  return noErr;
}

OSStatus slopay_target_macos_start(const slopay_target_macos_t *driver)
{
  if (driver == NULL || driver->audio_unit == NULL)
    return kAudio_ParamError;

  return AudioOutputUnitStart(driver->audio_unit);
}

OSStatus slopay_target_macos_stop(const slopay_target_macos_t *driver)
{
  if (driver == NULL || driver->audio_unit == NULL)
    return kAudio_ParamError;

  return AudioOutputUnitStop(driver->audio_unit);
}

void slopay_target_macos_cleanup(slopay_target_macos_t *driver)
{
  if (driver == NULL)
    return;

  if (driver->audio_unit != NULL) {
    AudioUnitUninitialize(driver->audio_unit);
    AudioComponentInstanceDispose(driver->audio_unit);
    driver->audio_unit = NULL;
  }

  driver->render   = NULL;
  driver->userdata = NULL;
}
