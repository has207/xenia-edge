/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_SYSTEM_H_
#define XENIA_APU_AUDIO_SYSTEM_H_

#include <atomic>
#include <mutex>

#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

constexpr fourcc_t kAudioSaveSignature = make_fourcc("XAUD");

class AudioDriver;
class XmaDecoder;

class AudioSystem {
 public:
  // TODO(gibbed): respect XAUDIO2_MAX_QUEUED_BUFFERS somehow (ie min(64,
  // XAUDIO2_MAX_QUEUED_BUFFERS))
  static constexpr size_t kMinimumQueuedFrames = 4;
  static constexpr size_t kMaximumQueuedFrames = 64;
  static constexpr uint32_t kAudioPumpInterval = 5333u;
  static constexpr uint32_t kAudioIntervalSlack = 400u;

  virtual ~AudioSystem();

  virtual std::string name() const = 0;

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  XmaDecoder* xma_decoder() const { return xma_decoder_.get(); }

  virtual X_STATUS Setup(kernel::KernelState* kernel_state);
  virtual void Shutdown();
  // Stops the worker that runs the guest audio callbacks. Idempotent, and
  // Shutdown does it too.
  void StopWorker();

  // A render client the host implements. The worker calls Pump on its guest
  // thread in place of a guest callback, at the same cadence and with the
  // same back-pressure so each Pump must submit one frame.
  class HostClient {
   public:
    virtual ~HostClient() = default;
    virtual void Pump(cpu::ThreadState* thread_state) = 0;
  };

  // Voice category volumes titles scale their voices by. Setting one marks
  // its category changed for every client. False for a category out of range.
  static constexpr uint32_t kVoiceCategoryCount = 32;
  bool SetVoiceCategoryVolume(uint32_t category, float volume);
  bool GetVoiceCategoryVolume(uint32_t category, float* out_volume);
  // The categories set since client |index| last asked, which this clears.
  // False for an index out of range.
  bool TakeVoiceCategoryChangeMask(size_t index, uint32_t* out_mask);

  X_STATUS RegisterClient(uint32_t callback, uint32_t callback_arg,
                          size_t* out_index);
  X_STATUS RegisterHostClient(HostClient* client, size_t* out_index);
  void UnregisterClient(size_t index);
  void SubmitFrame(size_t index, float* samples);

  // Get performance statistics for a client
  struct ClientPerformance {
    uint32_t frames_submitted;
    uint32_t frames_processed;
    uint32_t frames_dropped;
  };
  bool GetClientPerformance(size_t index, ClientPerformance* out_perf);

  // Creates an independent, non-registered driver instance.
  virtual AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                                    uint32_t frequency, uint32_t channels,
                                    bool need_format_conversion) = 0;

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  explicit AudioSystem(cpu::Processor* processor);

  virtual void Initialize();

  void WorkerThreadMain();

  virtual X_STATUS CreateDriver(size_t index,
                                xe::threading::Semaphore* semaphore,
                                AudioDriver** out_driver) = 0;
  X_STATUS CreateClientDriver(size_t index, xe::threading::Semaphore* semaphore,
                              AudioDriver** out_driver);
  void DestroyDriver(AudioDriver* driver);

  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;
  std::unique_ptr<XmaDecoder> xma_decoder_;
  uint32_t queued_frames_;

  std::atomic<bool> worker_running_ = {false};
  kernel::object_ref<kernel::XHostThread> worker_thread_;

  // Guards clients_. The global lock is never taken while holding it, so its
  // holders can't delay the pump.
  std::mutex clients_mutex_;
  static constexpr size_t kMaximumClientCount = 8;
  struct ClientSlot {
    AudioDriver* driver = nullptr;
    uint32_t callback = 0;
    uint32_t callback_arg = 0;
    uint32_t wrapped_callback_arg = 0;
    HostClient* host_client = nullptr;
    bool in_use = false;
    // Wall-clock deadline for this client's next pump.
    uint64_t next_pump_us = 0;
    // Every category counts as changed for a new client.
    uint32_t voice_category_change_mask = 0;
    std::atomic<uint32_t> frames_submitted{0};
    std::atomic<uint32_t> frames_processed{0};
    std::atomic<uint32_t> frames_dropped{0};
    // Held by worker during Execute; UnregisterClient waits on it.
    std::mutex callback_mutex;
  };
  ClientSlot clients_[kMaximumClientCount];
  // Guarded by clients_mutex_.
  float voice_category_volumes_[kVoiceCategoryCount];

  int FindFreeClient();
  X_STATUS AddClient(uint32_t callback, uint32_t callback_arg,
                     uint32_t wrapped_callback_arg, HostClient* host_client,
                     size_t* out_index);

  std::unique_ptr<xe::threading::Semaphore>
      client_semaphores_[kMaximumClientCount];
  // Wakes the worker to re-scan clients on register, shutdown, and pause.
  std::unique_ptr<xe::threading::Event> pending_work_event_;

  bool paused_ = false;
  threading::Fence pause_fence_;
  std::unique_ptr<threading::Event> resume_event_;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_AUDIO_SYSTEM_H_
