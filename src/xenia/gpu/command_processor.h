/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_COMMAND_PROCESSOR_H_
#define XENIA_GPU_COMMAND_PROCESSOR_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/gpu/xenos_zpd_report.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"

namespace xe {

class ByteStream;

namespace gpu {

enum class GPUSetting {
  ClearMemoryPageState,
  MemexportEnable,
  MemexportAwaitPixelExports,
  MemexportAwaitVertexExports,
  OcclusionQueryVIZ,
};

// Occlusion queries - ZPD report mode.
enum class ZPDMode {
  kFake,     // Fake counter walk, no real GPU queries (fake)
  kFast,     // Real queries, speculative writes biased visible (fast)
  kFastAlt,  // Fast, but replays cached zero deltas too (fast-alt)
  kStrict,   // Real queries, waits before writeback (strict)
};

void SaveGPUSetting(GPUSetting setting, uint64_t value);
bool GetGPUSetting(GPUSetting setting);

// Whether fences, interrupts and coherency requests wait for memory export from
// these shader stages. Other output holds back fences and interrupts where the
// backend can hold them.
bool IsMemexportAwaited(bool used_vertex, bool used_pixel);

// Shared pool capacity for D3D12 and Vulkan.
constexpr uint32_t kQueryPoolCapacity = 8192;

// Contiguous range of query indices for batched resolve/copy operations.
struct ResolveRange {
  uint32_t start;
  uint32_t count;
};

// Clock backstop for strict retire, triggered on the first failed guest wait.
constexpr uint64_t kStrictZPDRetireDeadlineMs = 2;
// The fast modes only need to keep queue growth in check.
constexpr uint64_t kFastZPDRetireDeadlineMs = 250;

// Cap for the fast-mode cached delta map.  Games reuse a small set of report
// addresses so this should never be hit, but prevents unbounded growth if a
// title cycles through unique addresses.  Clearing the cache has no
// correctness impact — it only removes speculative writeback hints.
constexpr size_t kFastZPDCacheMaxEntries = 1024;

class GraphicsSystem;
class Shader;

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  // Dimensions of the framebuffer textures. Should match window size.
  uint32_t width = 0;
  uint32_t height = 0;
  // Current front buffer, being drawn to the screen.
  uintptr_t front_buffer_texture = 0;
  // Current back buffer, being updated by the CP.
  uintptr_t back_buffer_texture = 0;
  // Backend data
  void* backend_data = nullptr;
  // Whether the back buffer is dirty and a swap is pending.
  bool pending = false;
};

enum class SwapMode {
  kNormal,
  kIgnored,
};

enum class GammaRampType {
  kUnknown = 0,
  kTable,
  kPWL,
};

class CommandProcessor {
 public:
  using ReportHandle = uint32_t;
  static constexpr ReportHandle kInvalidReportHandle = 0;

 protected:
  RingBuffer
      reader_;  // chrispy: instead of having ringbuffer on stack, have it near
                // the start of the class so we can access it via rel8. This
                // also reduces the number of params we need to pass
  // Converts the reader's host pointer (+ offset) to a guest physical address.
  uint32_t GuestReadPtrOffset(int32_t offset = 0) const;

 public:
  enum class SwapPostEffect {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system,
                   kernel::KernelState* kernel_state);
  virtual ~CommandProcessor();
  uint32_t counter() const { return counter_; }
  void increment_counter() { counter_++; }

  Shader* active_vertex_shader() const { return active_vertex_shader_; }
  Shader* active_pixel_shader() const { return active_pixel_shader_; }
  uint32_t active_vertex_shader_ucode_address() const {
    return active_vertex_shader_ucode_address_;
  }

  virtual bool Initialize();
  virtual void Shutdown();

  GraphicsSystem* graphics_system() const { return graphics_system_; }

  // Safe from any thread, even while the context is being torn down.
  std::string GetTitleStateSuffix() const;

  void CallInThread(std::function<void()> fn);

  // Dumps the EDRAM contents to a raw file.
  virtual bool DumpEdramSnapshotToFile(const std::filesystem::path& path) {
    return false;
  }

  virtual void ClearCaches();
  virtual void InvalidateGpuMemory();
  virtual void ClearReadbackBuffers();

  TraceWriter& trace_writer() { return trace_writer_; }

  // Whether resolve output reaches guest RAM when the CPU accesses it.
  bool IsReadbackResolveEnabled() const;

  // Get cached ZPD mode (avoids string parsing every frame).
  ZPDMode GetZPDMode() const { return cached_zpd_mode_; }
  // Set ZPD mode (updates both cvar and cached value).
  void SetZPDMode(ZPDMode mode);

  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect GetDesiredSwapPostEffect() const {
    return swap_post_effect_desired_;
  }
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  // Implementations must not make assumptions that the front buffer will
  // necessarily be a resolve destination - it may be a texture generated by any
  // means like written to by the CPU or loaded from a file (the disclaimer
  // screen right in the beginning of 4D530AA4 is not a resolved render target,
  // for instance).
  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height) {}

  // Throttle presentation based on framerate_limit cvar.
  // Called after IssueSwap to limit host frame rate without affecting guest
  // vblank timing.
  void ThrottlePresentation();

  // May be called not only from the command processor thread when the command
  // processor is paused, and the termination of this function may be explicitly
  // awaited.
  virtual void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> completion_callback = nullptr);

  virtual void RequestFrameTrace(const std::filesystem::path& root_path);
  // Sets the frame numbers to capture as frame traces, counted by the swaps
  // before them. Must be called before Initialize.
  void SetTraceFrameNumbers(const std::filesystem::path& root_path,
                            std::vector<uint32_t> frame_numbers);
  virtual void BeginTracing(const std::filesystem::path& root_path);
  virtual void EndTracing();
  // Safe from any thread, the writer is closed on the next swap.
  void RequestEndTracing() { trace_state_ = TraceState::kDisabled; }
  bool is_tracing_stream() const {
    return trace_state_ == TraceState::kStreaming;
  }

  virtual void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) = 0;

  // Shadowed by backends that route memory export through guest RAM (see
  // command_processor_memexport.inc). A no-op where export output never
  // reaches the CPU.
  void AwaitMemexportForCoherency(uint32_t base_bytes, uint32_t size_bytes) {}
  // Called before the command processor lets the guest see that the GPU got
  // past earlier commands - a fence, memory write, interrupt or scratch
  // register write-back - by backends that must have submitted what the guest
  // may then touch (see command_processor_resolve_readwatch.inc).
  virtual void SubmitResolvesForGuestSync() {}
  // The same, before raising a CP interrupt.
  virtual void SubmitResolvesForGuestInterrupt() {
    SubmitResolvesForGuestSync();
  }

  void RestoreRegisters(uint32_t first_register,
                        const uint32_t* register_values,
                        uint32_t register_count, bool execute_callbacks);
  void RestoreGammaRamp(
      const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
      const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
      uint32_t new_gamma_ramp_rw_component);
  virtual void RestoreEdramSnapshot(const void* snapshot) = 0;

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  void UpdateWritePointer(uint32_t value);

  void LogRegisterSet(uint32_t register_index, uint32_t value);
  void LogRegisterSets(uint32_t base_register_index, const uint32_t* values,
                       uint32_t n_values);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };

  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;

  // A staging buffer unused for this many frames is released.
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;

  // Progressive alignment for readback buffers to avoid wasting memory
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return xe::align(size, 256u * 1024u);  // 256KB for < 1MB
    } else if (size < 4 * 1024 * 1024) {
      return xe::align(size, 1u * 1024u * 1024u);  // 1MB for < 4MB
    } else {
      return xe::align(size, kReadbackBufferSizeIncrement);  // 16MB for >= 4MB
    }
  }

  // Generate a cache key for a specific resolve operation
  static inline uint64_t MakeReadbackResolveKey(uint32_t address,
                                                uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }

  // Constants for the shared resolve-downscale compute shader (used by the
  // D3D12 and Vulkan backends to downscale a scaled resolve back to 1x).
  struct ResolveDownscaleConstants {
    uint32_t scale_x;          // 1 to kMaxDrawResolutionScaleAlongAxis
    uint32_t scale_y;          // 1 to kMaxDrawResolutionScaleAlongAxis
    uint32_t pixel_size_log2;  // 0=8bit, 1=16bit, 2=32bit, 3=64bit
    uint32_t tile_count;       // Number of 32x32 tiles to process
    // Byte offset into the source buffer. On D3D12 this is 0 (the offset is
    // baked into the source SRV); on Vulkan it is the real byte offset.
    uint32_t source_offset_bytes;
    // When non-zero, apply half-pixel offset correction by sampling from
    // (scale/2, scale/2) within each scaled block instead of (0, 0).
    uint32_t half_pixel_offset;
  };

  void WorkerThreadMain();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;
  // Built once the context is set up, and kept until it shuts down.
  virtual std::string MakeTitleStateSuffix() const { return {}; }
  // rarely needed, most register writes have no special logic here
  XE_NOINLINE
  void HandleSpecialRegisterWrite(uint32_t index, uint32_t value);

  virtual void WriteRegister(uint32_t index, uint32_t value);

  // mem has big-endian register values
  XE_FORCEINLINE
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers);

  XE_FORCEINLINE
  virtual void WriteRegisterRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                          uint32_t num_registers);

  XE_NOINLINE
  void WriteOneRegisterFromRing(
      uint32_t base,
      uint32_t
          num_times);  // repeatedly write a value to one register, presumably a
                       // register with special handling for writes

  void WriteALURangeFromRing(xe::RingBuffer* ring, uint32_t base,
                             uint32_t num_times);

  void WriteFetchRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                               uint32_t num_times);

  void WriteBoolRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                              uint32_t num_times);

  void WriteLoopRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                              uint32_t num_times);

  void WriteREGISTERSRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                   uint32_t num_times);

  void WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                            uint32_t num_registers);

  void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                              uint32_t num_registers);

  void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers);

  void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers);

  void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                  uint32_t num_registers);

  const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table() const {
    return gamma_ramp_256_entry_table_;
  }
  const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb() const {
    return gamma_ramp_pwl_rgb_[0];
  }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  virtual void MakeCoherent();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();

  virtual void PollCompletedSubmission() {}

  // Used by strict ZPD to distinguish normal in flight latency from a
  // genuinely stuck report.
  virtual uint64_t GetCompletedSubmission() const { return 0; }

  // At a fence, interrupt or wait for idle: the submission whose completion
  // puts the export output ahead of it in guest RAM, or 0 once it is there,
  // which may take a wait. See command_processor_memexport.inc.
  virtual uint64_t TakeMemexportSignalSubmission() { return 0; }
  // Ends a submission held-back signals wait for if it is still being
  // recorded.
  virtual void EndSubmissionForHeldSignals(uint64_t submission) {}
  // Waits for a submission held-back signals wait for, ending it first if it is
  // still being recorded.
  virtual void AwaitSubmissionForHeldSignals(uint64_t submission) {}

  // Fences and interrupts, which the guest takes as the GPU being past the
  // commands before them. One is held back until export output ahead of it is
  // in guest RAM. Later ones wait behind it so the guest sees them in order.
  // value is in guest byte order. physical_address is where the fence lands,
  // whichever view destination points through.
  void WriteGuestFence(uint8_t* destination, uint32_t physical_address,
                       uint32_t value);
  void DispatchGuestInterrupt(uint32_t cpu_mask);
  // Performs held-back signals whose submission has completed, awaiting any
  // held for too long.
  void PerformCompletedHeldSignals();
  // Same, first ending the submission they wait for if it is still being
  // recorded, for when something may be waiting for one.
  void SubmitHeldSignals();
  // Performs held-back signals up to the last one writing physical_address,
  // waiting for the GPU so the command processor can read it.
  void AwaitHeldSignalsWriting(uint32_t physical_address);
  // Waits for export output and performs all held-back signals.
  void AwaitHeldSignalsAndMemexport();

  virtual void OnPrimaryBufferEnd() {}

  // TODO(boma): Add tracking for VIZ & EXT queries.
  enum class QueryOpenResult {
    kOpened,
    kDeferred,
    kPoolExhausted,
    kFailed,
  };

  // One EVENT_WRITE_ZPD. Measures the host query segments since the previous
  // report and owes the guest one write of the running counter. Reports
  // retire strictly in stream order.
  struct ZPDReport {
    ReportHandle handle = kInvalidReportHandle;
    // Set by the event that ends the measurement.
    uint32_t address = 0;
    // Guest sample counts. Each segment is normalized by its own scale area
    // when it resolves.
    XenosZPDReport delta;
    // Submission containing the most recently closed segment's resolve.
    uint64_t last_segment_end_submission = 0;
    uint32_t pending_segments = 0;
    // Fast modes write a guess at event time and correct it on retire.
    // The guessed delta is kept so later guesses can be re-based.
    XenosZPDReport speculative_value;
    XenosZPDReport speculative_delta;
    bool speculative = false;
    // For strict, when a report holds the D3D sentinel.
    // Only these are worth blocking a wait for.
    bool awaited = false;
  };

  static constexpr uint64_t kInvalidVIZGeneration = 0;
  struct VIZQueryHandle {
    uint32_t id = 0;
    uint64_t generation = kInvalidVIZGeneration;
  };

  // Host query segment open for the ZPD report and/or VIZ ID currently being
  // measured. Both read the same resolve.
  struct ActiveQuerySegment {
    uint32_t scale_area = 0;
    bool segment_active = false;
    bool segment_pending_begin = false;
    bool count_total = false;
    bool hybrid = false;
    bool report = false;
    VIZQueryHandle viz;
    bool survey = false;
    bool report_measuring() const {
      return segment_pending_begin || (segment_active && report);
    }
  };

  // Logged by the backend every 100 frames if ZPD logging cvar is true.
  struct ZPDStats {
    uint64_t reports_queued = 0;
    uint64_t reports_retired = 0;
    uint64_t segments_begun = 0;
    uint64_t segments_ended = 0;
    uint64_t pool_exhausted = 0;
    uint64_t failed = 0;
    // Speculative writes the real delta later disagreed with.
    uint64_t speculative_corrections = 0;
    // Reports force-retired on the backstop deadline with segments unresolved.
    uint64_t retires_abandoned = 0;
    uint64_t last_log_frame = 0;

    void Reset(uint64_t current_frame) {
      *this = {};
      last_log_frame = current_frame;
    }
  };

  virtual void EnsureQueryResources() {}
  virtual void ShutdownQueryResources() {}

  virtual bool IsQueryPoolReady() const { return false; }
  virtual bool CanOpenQuery() const { return true; }

  // Backend acquires a pool slot for the consumers in active_segment_,
  // records BeginQuery, tracks it internally.
  virtual QueryOpenResult OpenQuery(bool can_close_submission) {
    return QueryOpenResult::kFailed;
  }
  // Backend records EndQuery, queues a resolve for the active slot and the
  // VIZ predicate if there's a VIZ consumer. report_handle is invalid without
  // a report consumer.
  virtual bool CloseQuery(ReportHandle report_handle, const VIZQueryHandle& viz,
                          uint64_t& out_submission) {
    return false;
  }
  // Backend drains completed resolves and calls OnZPDQueryResolved and
  // OnVIZQueryResolved for each.
  virtual void PumpQueryResolves() {}
  // Backend waits for all pending segments of report_handle to resolve.
  virtual bool AwaitQueryResolve(ReportHandle report_handle,
                                 uint64_t wait_for_submission) {
    return false;
  }

  // Queues the current interval at report_address and starts the next one.
  void QueueZPDReport(uint32_t report_address);
  // Opens a new host query segment when CanOpenQuery is true.
  void OpenQuerySegment(bool can_close_submission);
  // Closes the current segment at a submission or render pass boundary.
  // The report and VIZ ID stay open and a new segment will open at the next
  // opportunity.
  void CloseQuerySegment();
  void EndZPDFrame() {
    CloseQuerySegment();
    active_segment_.segment_pending_begin = false;
  }
  // Splits the open segment when the draw scale or hybrid query Total counting
  // changes so each segment normalizes with one scale and counts one set of
  // draws, and when the VIZ ID or survey state changes. Also opens a pending
  // segment, once per draw.
  void UpdateQuerySegment(uint32_t scale_area, bool count_total, bool survey);

  // Called by backends when a host query resolve completes.
  // Accumulates the normalized sample counts into the report.
  void OnZPDQueryResolved(ReportHandle report_handle,
                          const XenosZPDReport& raw_counts,
                          uint32_t scale_area);
  // Queued or current report, nullptr once retired. Handles are issued in
  // order, so the queue is indexed by the front handle.
  ZPDReport* FindZPDReport(ReportHandle report_handle);
  // Handles a strict report the guest is waiting on while the ring is empty.
  void PrepareZPDForWait();
  // Called from PrepareForWait and submission boundaries so retired reports
  // reach the guest before it loops again. Fast modes give up on a stuck front
  // report after kFastZPDRetireDeadlineMs.
  void PumpPendingRetire();
  // Guest writeback.
  void WriteZPDReport(uint32_t report_address, const XenosZPDReport& value) {
    value.WriteTo(
        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
            report_address));
  }
  void ResetZPDState() {
    zpd_mode_ = GetZPDMode();
    active_segment_ = {};
    zpd_next_report_handle_ = 1;
    zpd_current_report_ = {};
    zpd_reports_.clear();
    zpd_awaited_report_count_ = 0;
    fast_zpd_report_cached_deltas_.clear();
    zpd_sample_counter_ = {};
    zpd_speculative_sample_counter_ = {};
    fake_zpd_sample_count_ = 0;
    zpd_pending_retire_start_ms_ = 0;
    zpd_force_fake_fallback_ = false;
  }

  // VIZ_QUERY has the scan converter track 64 query IDs. An ID is visible when
  // its geometry is still potentially visible after hi-Z. Without any
  // hierarchical state, we get answers from the survey's query segment instead.
  // Draws carrying a VIZ token then get predicated on the GPU. Anything
  // unmeasured, for whatever reason, stays visible.
  struct VIZQuery {
    uint64_t generation = kInvalidVIZGeneration;
    uint32_t pending_segments = 0;
    bool resolved = true;
    bool visible = true;
    bool active = false;
    // Survey reached the backend during this generation.
    bool surveyed = false;
    // OR of the resolved segments for this generation.
    bool accumulated_visible = false;
    // Something went wrong while measuring; this doesn't mean not-visible.
    bool fallback = false;
    uint64_t last_segment_end_submission = 0;
    // The backend has a survey result ready to use for predication.
    bool predicate_armed = false;
    // Once the predicate no longer covers the full unresolved query,
    // later segments can't make it exact again.
    bool predicate_blocked = false;
  };

  // Surveys ride the query segments as a consumer, see ActiveQuerySegment.
  void BeginVIZQuery(uint32_t id);
  void EndVIZQuery(uint32_t id);
  // Backend calls this for every draw under an active ID. Measured only if it
  // ran inside a segment carrying the ID.
  void OnVIZSurveyDraw(bool measured);
  // Backend reports a resolved segment here.
  void OnVIZQueryResolved(uint32_t id, uint64_t generation, bool visible);
  // Whether a draw with a VIZ token runs. If it does, viz_draw_predicate_
  // is set when the backend has to predicate it. Memexport and copy draws
  // pass through untouched, since a cull or a predicate would lose their side
  // effects.
  bool PrepareVIZDraw(uint32_t token);
  // Drops every answer when occlusion_query_viz was switched since the last
  // call, since those from before the switch are stale either way.
  void SyncVIZEnabled();
  // Backend waits for the submission holding a VIZ segment's resolve.
  virtual void AwaitVIZQueryResolve(uint64_t wait_for_submission) {}

  // The backend stages a survey's result into its predicate buffer while the
  // generation can still use it, and arms or blocks the ID accordingly.
  bool CanArmVIZPredicate(uint32_t id, uint64_t generation) const {
    const VIZQuery& query = viz_queries_[id];
    return query.generation == generation && !query.predicate_armed &&
           !query.predicate_blocked;
  }
  void ArmVIZPredicate(uint32_t id, uint64_t generation) {
    VIZQuery& query = viz_queries_[id];
    assert_true(query.generation == generation);
    assert_false(query.predicate_armed);
    assert_false(query.predicate_blocked);
    query.predicate_armed = true;
  }
  void BlockVIZPredicate(uint32_t id, uint64_t generation) {
    VIZQuery& query = viz_queries_[id];
    assert_true(query.generation == generation);
    query.predicate_armed = false;
    query.predicate_blocked = true;
  }
  // Whether the draw being issued runs under an armed predicate.
  bool IsVIZPredicateArmed() const {
    return viz_draw_predicate_.generation != kInvalidVIZGeneration &&
           viz_queries_[viz_draw_predicate_.id].predicate_armed;
  }
  void ResetVIZState() {
    active_segment_.viz = {};
    viz_draw_predicate_ = {};
    viz_pending_resolves_ = 0;
    for (VIZQuery& query : viz_queries_) {
      query = {};
    }
  }

#include "pm4_command_processor_declare.h"

  virtual Shader* LoadShader(xenos::ShaderType shader_type,
                             const uint32_t* host_address,
                             uint32_t dword_count) {
    return nullptr;
  }

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                         IndexBufferInfo* index_buffer_info,
                         bool major_mode_explicit) {
    return false;
  }
  virtual bool IssueCopy() { return false; }

  // Debug marker stubs for base class (overridden by D3D12/Vulkan backends).
  bool debug_markers_enabled() const { return false; }
  void PushDebugMarker(const char* format, ...) {}
  void PopDebugMarker() {}
  void InsertDebugMarker(const char* format, ...) {}

  // "Actual" is for the command processor thread, to be read by the
  // implementations.
  SwapPostEffect GetActualSwapPostEffect() const {
    return swap_post_effect_actual_;
  }

  virtual void InitializeTrace();
  // Saves the guest output of the frame that was just traced next to the trace
  // itself, as ground truth for what a replay of it should produce.
  void WriteTraceFrameScreenshot();

  Memory* memory_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  RegisterFile* XE_RESTRICT register_file_ = nullptr;

  struct HeldGuestSignal {
    // Has to complete before this is performed.
    uint64_t submission = 0;
    // Null for an interrupt.
    uint8_t* destination = nullptr;
    uint32_t physical_address = UINT32_MAX;
    // In guest byte order, or the CPU mask of an interrupt.
    uint32_t value = 0;
    std::chrono::steady_clock::time_point held_since;
  };
  std::deque<HeldGuestSignal> held_guest_signals_;
  // The latest submission a held-back signal waits for.
  uint64_t held_guest_signals_submission_ = 0;
  void HoldOrPerformGuestSignal(const HeldGuestSignal& signal);
  void PerformGuestSignal(const HeldGuestSignal& signal);
  // Performs the first await_count held-back signals, waiting for the GPU, and
  // any after them whose submission has completed or that were held for too
  // long, awaiting those.
  void PerformHeldSignals(size_t await_count);

  ZPDMode zpd_mode_ = ZPDMode::kFast;

  ReportHandle zpd_next_report_handle_ = 1;
  // The report the next event will end. No handle means nothing is measuring.
  ZPDReport zpd_current_report_;
  ActiveQuerySegment active_segment_{};
  bool query_segment_opening_ = false;
  // Reports owed to the guest, in stream order.
  std::deque<ZPDReport> zpd_reports_;
  // Strict reports containing the D3D sentinel the guest polls.
  uint32_t zpd_awaited_report_count_ = 0;

  // The retired counter advances as intervals retire. The speculative counter
  // tracks the latest queued report so fast modes can monotonically write
  // increasing values at EVENT_WRITE_ZPD, using each report's last retired
  // delta as its next speculative prediction.
  XenosZPDReport zpd_sample_counter_;
  XenosZPDReport zpd_speculative_sample_counter_;
  std::unordered_map<uint32_t, XenosZPDReport> fast_zpd_report_cached_deltas_;

  // Sticky after host pool init failure. Forces EVENT_WRITE_ZPD onto the fake
  // path so guests don't stall waiting on a pending sentinel that will never
  // be written. Cleared by ResetZPDState.
  bool zpd_force_fake_fallback_ = false;
  uint32_t fake_zpd_sample_count_ = 0;

  // Uptime in ms when the current retire backstop was armed.
  uint64_t zpd_pending_retire_start_ms_ = 0;

  // Set by the backend when resolution scale changes.
  uint32_t zpd_draw_resolution_scale_x_ = 1;
  uint32_t zpd_draw_resolution_scale_y_ = 1;

  // Scale area for the segment being closed.
  uint32_t GetZPDScaleArea() const {
    return active_segment_.scale_area
               ? active_segment_.scale_area
               : zpd_draw_resolution_scale_x_ * zpd_draw_resolution_scale_y_;
  }

  ZPDStats zpd_stats_;

  // Represents the 64 slots of SC_VIZ_QUERY_STATUS_0/1.
  std::array<VIZQuery, 64> viz_queries_{};
  // Predicate for the draw being issued, set by PM4.
  VIZQueryHandle viz_draw_predicate_{};
  // Segments with an unresolved VIZ consumer.
  uint32_t viz_pending_resolves_ = 0;
  // occlusion_query_viz as of the last SyncVIZEnabled.
  bool viz_enabled_ = false;

  TraceWriter trace_writer_;
  enum class TraceState {
    kDisabled,
    kStreaming,
    kSingleFrame,
  };
  TraceState trace_state_ = TraceState::kDisabled;
  std::filesystem::path trace_stream_path_;
  std::filesystem::path trace_frame_path_;
  // Full path of the frame trace currently being written, so the reference
  // screenshot can be saved beside it when the frame closes.
  std::filesystem::path trace_frame_file_path_;
  // Frame numbers passed to SetTraceFrameNumbers, and the number of swaps
  // executed so far that they are compared against (counter_ also counts
  // vblanks, so it skips numbers between swaps).
  std::vector<uint32_t> trace_frame_numbers_;
  uint32_t trace_swap_count_ = 0;

  std::atomic<bool> worker_running_;
  kernel::object_ref<kernel::XHostThread> worker_thread_;

  std::queue<std::function<void()>> pending_fns_;

  // MicroEngine binary from PM4_ME_INIT
  std::vector<uint32_t> me_bin_;

  uint32_t counter_ = 0;

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;

  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_update_freq_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;

  std::unique_ptr<xe::threading::Event> write_ptr_index_event_;
  std::atomic<uint32_t> write_ptr_index_;

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;
  // Guest physical address the active vertex shader's ucode was loaded from,
  // for reading it back from shared memory (the ucode interpreter placeholder).
  // 0 if unknown (loaded immediately, embedded in the command buffer).
  uint32_t active_vertex_shader_ucode_address_ = 0;

  bool paused_ = false;

  // By default (such as for tools), post-processing is disabled.
  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

  // Cached ZPD occlusion query mode (defaults to fake)
  ZPDMode cached_zpd_mode_ = ZPDMode::kFake;

  // For host frame rate limiting at IssueSwap
  uint64_t last_swap_time_ = 0;

 private:
  void SetTitleStateSuffix(std::string suffix);

  reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  uint32_t gamma_ramp_rw_component_ = 0;

  mutable std::mutex title_state_suffix_mutex_;
  std::string title_state_suffix_;

  XE_NOINLINE XE_COLD void LogKickoffInitator(uint32_t value);
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_COMMAND_PROCESSOR_H_
