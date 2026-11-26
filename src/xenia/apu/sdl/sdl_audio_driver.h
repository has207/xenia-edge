/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_
#define XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_

#include <mutex>
#include <queue>
#include <stack>

#include <SDL3/SDL.h>
#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(xe::threading::Semaphore* semaphore,
                 uint32_t frequency = kFrameFrequencyDefault,
                 uint32_t channels = kFrameChannelsDefault,
                 bool need_format_conversion = true);
  ~SDLAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  void SetVolume(float volume) override { volume_ = volume; };
  void Shutdown() override;

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream,
                          int additional_amount, int total_amount);

  xe::threading::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;

  float volume_ = 1.0f;

  uint32_t frame_frequency_;
  uint32_t frame_channels_;
  uint32_t channel_samples_;
  uint32_t frame_size_;
  bool need_format_conversion_;
  // Guest is 5.1 and device is stereo: do the downmix here, not in SDL3.
  bool manual_downmix_5_1_to_stereo_ = false;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SDL_SDL_AUDIO_DRIVER_H_
