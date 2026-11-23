/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GRAPHICS_SYSTEM_H_
#define XENIA_GPU_GRAPHICS_SYSTEM_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/base/delegate.h"
#include "xenia/cpu/processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
class Emulator;
}  // namespace xe

namespace xe {
namespace gpu {

constexpr std::array<std::pair<uint16_t, uint16_t>, 17>
    internal_display_resolution_entries = {{{640, 480},
                                            {640, 576},
                                            {720, 480},
                                            {720, 576},
                                            {800, 600},
                                            {848, 480},
                                            {1024, 768},
                                            {1152, 864},
                                            {1280, 720},
                                            {1280, 768},
                                            {1280, 960},
                                            {1280, 1024},
                                            {1360, 768},
                                            {1440, 900},
                                            {1680, 1050},
                                            {1920, 540},
                                            {1920, 1080}}};

constexpr std::array<std::pair<uint16_t, uint16_t>, 3>
    driver_display_resolution = {{{1440, 900}, {1280, 720}, {1680, 1050}}};

class CommandProcessor;

class GraphicsSystem {
 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  kernel::KernelState* kernel_state() const { return kernel_state_; }
  ui::GraphicsProvider* provider() const { return provider_.get(); }
  ui::Presenter* presenter() const { return presenter_.get(); }

  virtual X_STATUS Setup(cpu::Processor* processor,
                         kernel::KernelState* kernel_state,
                         ui::WindowedAppContext* app_context,
                         bool with_presentation);
  virtual void Shutdown();
  // Stops the vblank thread. Idempotent, and Shutdown does it too.
  void StopFrameLimiter();

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return register_file_; }
  CommandProcessor* command_processor() const {
    return command_processor_.get();
  }

  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  virtual void EnableReadPointerWriteBack(uint32_t ptr,
                                          uint32_t block_size_log2);

  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data);
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  virtual void ClearCaches();

  void InvalidateGpuMemory();

  // started_callback runs on the thread doing the loading right before it
  // starts, which may be well after this call while the command processor is
  // still setting up.
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> started_callback = nullptr,
      std::function<void()> completion_callback = nullptr);

  void RequestFrameTrace();
  void BeginTracing();
  void EndTracing();
  void RequestEndTracing();
  bool is_tracing_stream() const;

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  static std::pair<uint16_t, uint16_t> GetInternalDisplayResolution();

  std::pair<uint32_t, uint32_t> GetScaledAspectRatio() const {
    return {scaled_aspect_x_, scaled_aspect_y_};
  };
  void SetScaledAspectRatio(uint32_t x, uint32_t y) {
    scaled_aspect_x_ = x;
    scaled_aspect_y_ = y;
  };

  // Command processor thread, once per guest swap.
  void OnGuestSwap();
  // Measured from the guest's swaps, zero once it swaps no frame for a second.
  float GetFramesPerSecond() const;

  // Whether pipelines are being created, in the background or inline, from
  // the thread that started or finished them.
  xe::Delegate<bool> on_shader_compilation;

 protected:
  GraphicsSystem();

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                    uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                 uint32_t addr, uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<ui::GraphicsProvider> provider_;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> frame_limiter_worker_running_;
  kernel::object_ref<kernel::XHostThread> frame_limiter_worker_thread_;

  // Anchors for synthesizing D1MODE_V_COUNTER. last_vblank_guest_tick_ is
  // the guest tick at the most recent MarkVblank; vblank_period_ticks_ is
  // the most recent measured interval between MarkVblank calls. Both are
  // 0 before the first vblank fires.
  std::atomic<uint64_t> last_vblank_guest_tick_{0};
  std::atomic<uint64_t> vblank_period_ticks_{0};

  RegisterFile* register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

  uint32_t scaled_aspect_x_ = 0;
  uint32_t scaled_aspect_y_ = 0;

 private:
  static constexpr uint64_t kFrameRatePeriodMs = 500;
  // A longer gap between swaps reads as zero and restarts the period.
  static constexpr uint64_t kFrameRateStallMs = 1000;

  // Command processor thread only.
  uint64_t frame_rate_period_start_ticks_ = 0;
  uint64_t frame_rate_period_swaps_ = 0;

  std::atomic<uint64_t> frame_rate_last_swap_ticks_{0};
  std::atomic<float> frame_rate_frames_per_second_{0.0f};

  std::unique_ptr<ui::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GRAPHICS_SYSTEM_H_
