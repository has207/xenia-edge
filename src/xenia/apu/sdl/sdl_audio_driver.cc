/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_audio_driver.h"

#include <cstring>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/helper/sdl/sdl_helper.h"

namespace xe {
namespace apu {
namespace sdl {

SDLAudioDriver::SDLAudioDriver(xe::threading::Semaphore* semaphore,
                               uint32_t frequency, uint32_t channels,
                               bool need_format_conversion)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  switch (frame_channels_) {
    case 6:
      channel_samples_ = 256;
      break;
    case 2:
      channel_samples_ = 768;
      break;
    default:
      assert_unhandled_case(frame_channels_);
  }
  frame_size_ = sizeof(float) * frame_channels_ * channel_samples_;
  assert_true(frame_size_ <= kFrameSizeMax);
  assert_true(!need_format_conversion_ || frame_channels_ == 6);
}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
};

bool SDLAudioDriver::Initialize() {
  if (!xe::helper::sdl::SDLHelper::Prepare()) {
    return false;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return false;
  }
  sdl_initialized_ = true;

  // SDL3 always converts between the stream spec and the device spec, so we
  // just request our native channel count and let SDL3 downmix/upmix to the
  // hardware's actual configuration.
  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(frame_channels_);
  spec.freq = static_cast<int>(frame_frequency_);

  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, SDLCallback, this);
  if (!sdl_stream_) {
    XELOGE("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  // Downmix 5.1->stereo ourselves; SDL3's matrix mixer normalizes off ~6dB.
  // Shared downmix expects BE-sequential input, so only engage with the
  // format-conversion path (i.e. main audio system, not XMP-stereo).
  if (frame_channels_ == 6 && need_format_conversion_) {
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(sdl_stream_);
    SDL_AudioSpec device_spec = {};
    int sample_frames = 0;
    if (dev != 0 &&
        SDL_GetAudioDeviceFormat(dev, &device_spec, &sample_frames) &&
        device_spec.channels == 2) {
      SDL_AudioSpec stereo_spec = spec;
      stereo_spec.channels = 2;
      if (SDL_SetAudioStreamFormat(sdl_stream_, &stereo_spec, nullptr)) {
        manual_downmix_5_1_to_stereo_ = true;
      } else {
        XELOGW("SDL_SetAudioStreamFormat() failed: {}", SDL_GetError());
      }
    }
  }

  SDL_ResumeAudioStreamDevice(sdl_stream_);

  return true;
}

void SDLAudioDriver::SubmitFrame(float* frame) {
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_channels_ * channel_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, frame, frame_size_);

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    // SDL_OpenAudioDeviceStream-created streams own the underlying logical
    // device and close it on destroy.
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  };
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  };
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream,
                                 int additional_amount, int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream || additional_amount <= 0) {
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int frame_bytes = static_cast<int>(driver->frame_size_);

  // SDL3 pulls until additional_amount is satisfied or we run out of frames.
  // If the queue empties, returning early lets SDL3 fill the remainder with
  // silence — the producer will catch up on the next callback.
  while (additional_amount > 0) {
    float* buffer;
    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      if (driver->frames_queued_.empty()) {
        return;
      }
      buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
    }

    // Produce a scratch buffer in SDL3's expected format. With manual downmix
    // we go straight from BE-sequential 5.1 to interleaved LE stereo via the
    // shared conversion; otherwise feed SDL3 our native channel count and let
    // its matrix mixer handle any conversion to the device's actual format.
    float scratch[kFrameSizeMax / sizeof(float)];
    int out_bytes;
    if (driver->manual_downmix_5_1_to_stereo_) {
      conversion::sequential_6_BE_to_interleaved_2_LE(scratch, buffer,
                                                      driver->channel_samples_);
      out_bytes =
          static_cast<int>(driver->channel_samples_ * 2 * sizeof(float));
    } else if (driver->need_format_conversion_) {
      conversion::sequential_6_BE_to_interleaved_6_LE(scratch, buffer,
                                                      driver->channel_samples_);
      out_bytes = frame_bytes;
    } else {
      std::memcpy(scratch, buffer, frame_bytes);
      out_bytes = frame_bytes;
    }

    // Scale by master (cvar) and per-driver volume.
    const uint32_t mv = cvars::volume > 100 ? 100 : cvars::volume;
    const float volume = driver->volume_ * (mv / 100.0f);
    if (volume != 1.0f) {
      const size_t count = out_bytes / sizeof(float);
      for (size_t i = 0; i < count; ++i) {
        scratch[i] *= volume;
      }
    }

    SDL_PutAudioStreamData(stream, scratch, out_bytes);
    additional_amount -= out_bytes;

    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      driver->frames_unused_.push(buffer);
    }

    auto ret = driver->semaphore_->Release(1, nullptr);
    assert_true(ret);
  }
}
}  // namespace sdl
}  // namespace apu
}  // namespace xe
