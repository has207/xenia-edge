/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <mutex>

#include "xenia/apu/audio_system.h"
#include "xenia/base/clock.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The kernel keeps the active speaker configuration in a global that starts
// out as the dashboard audio setting; XAudioSetSpeakerConfig replaces it for
// everyone in the session (media players switch to stereo for stereo content
// and put the user's setting back afterwards).
static std::atomic<bool> speaker_config_overridden_{false};
static std::atomic<uint32_t> speaker_config_override_{0};

dword_result_t XAudioGetSpeakerConfig_entry(lpdword_t config_ptr) {
  if (!config_ptr) {
    return X_E_INVALIDARG;
  }
  if (speaker_config_overridden_) {
    *config_ptr = speaker_config_override_.load();
    return X_ERROR_SUCCESS;
  }
  kernel_state()->xconfig()->ReadSetting(
      XCONFIG_USER_CATEGORY,
      XCONFIG_USER_CATEGORY_ENTRIES::XCONFIG_USER_AUDIO_FLAGS, config_ptr);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetSpeakerConfig, kAudio, kImplemented);

dword_result_t XAudioSetSpeakerConfig_entry(dword_t config) {
  // Host output keeps its own channel layout; the guest mixes to 5.1 frames
  // regardless, so only the value reported back to titles changes.
  speaker_config_override_ = config;
  speaker_config_overridden_ = true;
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioSetSpeakerConfig, kAudio, kImplemented);

// Ducker settings. The getters return a float in f1. Nothing in the host mixer
// ducks, so these are only the values reported to titles that run their own
// ducking from them (XamlHost does). The release time is used as a divisor.
struct XAudioDuckerSettings {
  float level = 0.25f;         // Linear gain while ducked.
  float threshold = 0.0001f;   // Mean square level that triggers ducking.
  float attack_time = 0.01f;   // Seconds.
  float hold_time = 0.5f;      // Seconds.
  float release_time = 0.25f;  // Seconds.
};
constexpr XAudioDuckerSettings kDuckerSettings{};

static void StoreFloatResult(PPCContext* ctx, float value) {
  ctx->f[1] = static_cast<double>(value);
}

void XAudioGetDuckerLevel_entry(const ppc_context_t& ctx) {
  StoreFloatResult(ctx, kDuckerSettings.level);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerLevel, kAudio, kStub, kSketchy);

void XAudioGetDuckerThreshold_entry(const ppc_context_t& ctx) {
  StoreFloatResult(ctx, kDuckerSettings.threshold);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerThreshold, kAudio, kStub, kSketchy);

void XAudioGetDuckerAttackTime_entry(const ppc_context_t& ctx) {
  StoreFloatResult(ctx, kDuckerSettings.attack_time);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerAttackTime, kAudio, kStub, kSketchy);

void XAudioGetDuckerHoldTime_entry(const ppc_context_t& ctx) {
  StoreFloatResult(ctx, kDuckerSettings.hold_time);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerHoldTime, kAudio, kStub, kSketchy);

void XAudioGetDuckerReleaseTime_entry(const ppc_context_t& ctx) {
  StoreFloatResult(ctx, kDuckerSettings.release_time);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetDuckerReleaseTime, kAudio, kStub, kSketchy);

dword_result_t XAudioGetRenderDriverTic_entry() {
  // Count of render driver frames (256 samples at 48 kHz, every 5.333 ms)
  // since boot, wrapped the way the kernel wraps it (mod 0xFFFF). Derived from
  // the guest clock so it follows guest time scaling like the audio pump does.
  const uint64_t frequency = Clock::guest_tick_frequency();
  if (!frequency) {
    return 0;
  }
  // 48000 / 256 = 187.5 frames per second.
  const uint64_t frames = Clock::QueryGuestTickCount() * 375 / (frequency * 2);
  return static_cast<uint32_t>(frames % 0xFFFF);
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetRenderDriverTic, kAudio, kImplemented,
                         kHighFrequency);

// MEC (multichannel echo cancellation) clients get a callback on the audio
// thread for every rendered frame and copy the final mix with
// XAudioCaptureRenderDriverFrame as the echo reference. Xenia's mixer runs on
// the host, so registration is tracked but no callbacks are delivered (they
// only matter with a microphone, which is never attached) and the captured
// reference is silence.
constexpr uint32_t kMecClientHandleBase = 0x414D0000;
constexpr size_t kMaximumMecClientCount = 4;
// One render driver frame: 256 samples for each of 6 channels.
constexpr size_t kRenderDriverFrameSize = 256 * 6 * sizeof(float);
struct MecClientSlot {
  uint32_t callback = 0;
  uint32_t callback_arg = 0;
  bool in_use = false;
};
static std::mutex mec_clients_mutex_;
static MecClientSlot mec_clients_[kMaximumMecClientCount];

static bool IsValidMecClient(uint32_t handle) {
  if ((handle & 0xFFFF0000) != kMecClientHandleBase) {
    return false;
  }
  const size_t index = handle & 0x0000FFFF;
  std::lock_guard<std::mutex> lock(mec_clients_mutex_);
  return index < kMaximumMecClientCount && mec_clients_[index].in_use;
}

dword_result_t XAudioRegisterRenderDriverMECClient_entry(lpdword_t callback_ptr,
                                                         lpdword_t driver_ptr) {
  if (!callback_ptr || !driver_ptr) {
    return X_E_INVALIDARG;
  }
  const uint32_t callback = callback_ptr[0];
  if (!callback) {
    return X_E_INVALIDARG;
  }

  std::lock_guard<std::mutex> lock(mec_clients_mutex_);
  for (size_t i = 0; i < kMaximumMecClientCount; ++i) {
    auto& slot = mec_clients_[i];
    if (slot.in_use) {
      continue;
    }
    slot.callback = callback;
    slot.callback_arg = callback_ptr[1];
    slot.in_use = true;
    *driver_ptr = kMecClientHandleBase | static_cast<uint32_t>(i);
    return X_ERROR_SUCCESS;
  }
  return X_E_FAIL;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioRegisterRenderDriverMECClient, kAudio, kStub,
                         kSketchy);

dword_result_t XAudioUnregisterRenderDriverMECClient_entry(
    lpunknown_t driver_ptr) {
  const uint32_t handle = driver_ptr.guest_address();
  if (!IsValidMecClient(handle)) {
    return X_E_INVALIDARG;
  }
  std::lock_guard<std::mutex> lock(mec_clients_mutex_);
  mec_clients_[handle & 0x0000FFFF] = MecClientSlot();
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioUnregisterRenderDriverMECClient, kAudio, kStub);

dword_result_t XAudioCaptureRenderDriverFrame_entry(lpunknown_t driver_ptr,
                                                    lpvoid_t samples_ptr) {
  if (!IsValidMecClient(driver_ptr.guest_address()) || !samples_ptr) {
    return X_E_INVALIDARG;
  }
  samples_ptr.Zero(kRenderDriverFrameSize);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioCaptureRenderDriverFrame, kAudio, kStub,
                         kSketchy);

// Structure for XAudioQueryDriverPerformance
struct XAudioDriverPerformance {
  xe::be<uint32_t> frames_submitted;
  xe::be<uint32_t> frames_processed;
  xe::be<uint32_t> frames_dropped;
  xe::be<uint32_t> latency_ms;
  xe::be<uint32_t> reserved[4];
};

dword_result_t XAudioQueryDriverPerformance_entry(
    lpunknown_t driver_ptr, pointer_t<XAudioDriverPerformance> perf_ptr) {
  if (!driver_ptr || !perf_ptr) {
    return X_E_INVALIDARG;
  }

  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  size_t client_index = driver_ptr.guest_address() & 0x0000FFFF;
  auto audio_system = kernel_state()->emulator()->audio_system();

  // Get real performance metrics from the audio system
  apu::AudioSystem::ClientPerformance perf;
  if (audio_system->GetClientPerformance(client_index, &perf)) {
    perf_ptr->frames_submitted = perf.frames_submitted;
    perf_ptr->frames_processed = perf.frames_processed;
    perf_ptr->frames_dropped = perf.frames_dropped;
  } else {
    // Client not found, return zeros
    perf_ptr->frames_submitted = 0;
    perf_ptr->frames_processed = 0;
    perf_ptr->frames_dropped = 0;
  }

  // Calculate latency based on queued frames
  // At 48kHz with 256 samples per frame for 6-channel audio:
  // Frame duration = 256 samples / 48000 Hz = ~5.33ms per frame
  // With default 8 queued frames: 8 * 5.33ms = ~42.67ms latency
  uint32_t queued_frames = perf.frames_submitted - perf.frames_processed;
  constexpr float kFrameDurationMs = 256.0f / 48000.0f * 1000.0f;  // ~5.33ms
  perf_ptr->latency_ms =
      static_cast<uint32_t>(queued_frames * kFrameDurationMs);

  for (int i = 0; i < 4; i++) {
    perf_ptr->reserved[i] = 0;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioQueryDriverPerformance, kAudio, kStub);

dword_result_t XAudioGetVoiceCategoryVolumeChangeMask_entry(
    lpunknown_t driver_ptr, lpdword_t out_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  xe::threading::MaybeYield();

  // Checking these bits to see if any voice volume changed.
  // I think.
  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolumeChangeMask, kAudio, kStub,
                         kHighFrequency);

dword_result_t XAudioGetVoiceCategoryVolume_entry(dword_t unk,
                                                  lpfloat_t out_ptr) {
  // Expects a floating point single. Volume %?
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolume, kAudio, kStub,
                         kHighFrequency);

dword_result_t XAudioEnableDucker_entry(dword_t unk) { return X_ERROR_SUCCESS; }
DECLARE_XBOXKRNL_EXPORT1(XAudioEnableDucker, kAudio, kStub);

dword_result_t XAudioRegisterRenderDriverClient_entry(lpdword_t callback_ptr,
                                                      lpdword_t driver_ptr) {
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  uint32_t callback_arg = callback_ptr[1];

  auto audio_system = kernel_state()->emulator()->audio_system();

  size_t index;
  auto result = audio_system->RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    return result;
  }

  assert_true(!(index & ~0x0000FFFF));
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioRegisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioUnregisterRenderDriverClient_entry(
    lpunknown_t driver_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto audio_system = kernel_state()->emulator()->audio_system();
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioUnregisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioSubmitRenderDriverFrame_entry(lpunknown_t driver_ptr,
                                                   lpunknown_t samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto audio_system = kernel_state()->emulator()->audio_system();
  auto samples =
      kernel_state()->memory()->TranslateVirtual<float*>(samples_ptr);
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples);

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioSubmitRenderDriverFrame, kAudio, kImplemented,
                         kHighFrequency);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Audio);
