/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/d3d12_command_processor.h"

#include <cstdarg>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/gpu/d3d12/d3d12_zpd_query_pool.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/d3d12/d3d12_presenter.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "xenia/gpu/spirv_fsi_system_constants.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"

DEFINE_bool(d3d12_bindless, true,
            "Use bindless resources where available - may improve performance, "
            "but may make debugging more complicated.",
            "D3D12");

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(d3d12_debug);
DECLARE_bool(gpu_debug_markers);
DECLARE_bool(memexport_enable);
DECLARE_bool(submit_on_primary_buffer_end);

namespace xe {
namespace gpu {
namespace d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/apply_gamma_pwl_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/apply_gamma_pwl_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/apply_gamma_table_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/apply_gamma_table_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/fxaa_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/fxaa_extreme_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_dxil/resolve_downscale_cs.h"
}  // namespace shaders

D3D12CommandProcessor::D3D12CommandProcessor(
    D3D12GraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      deferred_command_list_(*this) {}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  // Enable debug markers if the CVAR is set or RenderDoc is detected.
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  shared_memory_->InvalidateAllPages();
}

void D3D12CommandProcessor::ClearReadbackBuffers() {
  if (AwaitAllQueueOperationsCompletion()) {
    ClearReadbackStagingBuffers();
    // Their staging buffers are gone, so there is nothing left to copy out.
    memexport_staged_.clear();
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking,
                                           std::move(completion_callback));
}

void D3D12CommandProcessor::RequestFrameTrace(
    const std::filesystem::path& root_path) {
  // Capture with PIX if attached.
  if (GetD3D12Provider().GetGraphicsAnalysis() != nullptr) {
    pix_capture_requested_.store(true, std::memory_order_relaxed);
    return;
  }
  CommandProcessor::RequestFrameTrace(root_path);
}

void D3D12CommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                     uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
}

void D3D12CommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  // Starting a new frame because descriptors may be needed.
  if (!BeginSubmission(true)) {
    return;
  }
  render_target_cache_->RestoreEdramSnapshot(snapshot);
}

void D3D12CommandProcessor::PollCompletedSubmission() {
  // Strict ZPD just needs the completion timeline updated and any ready query
  // resolves drained here.
  completion_timeline_->AwaitSubmissionAndUpdateCompleted(0);
  PumpQueryResolves();
}

bool D3D12CommandProcessor::PushTransitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES old_state,
    D3D12_RESOURCE_STATES new_state, UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update,
    uint32_t count_for_full_update, D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update,
      count_for_full_update, descriptor_index);
  if (current_heap_index ==
      ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(
    uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count > kViewBindlessHeapSize - view_bindless_heap_allocated_ +
                    view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.emplace_back(descriptor_index,
                                                      GetCurrentSubmission());
      handles_out[i] =
          std::make_pair(provider.OffsetViewDescriptor(
                             view_bindless_heap_cpu_start_, descriptor_index),
                         provider.OffsetViewDescriptor(
                             view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(
            ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count, count,
            cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] =
          std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                         provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(provider.OffsetViewDescriptor(
                            view_bindless_heap_cpu_start_, uint32_t(view)),
                        provider.OffsetViewDescriptor(
                            view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update,
    uint32_t count_for_full_update, D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out,
    D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update,
      count_for_full_update, descriptor_index);
  if (current_heap_index ==
      ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(),
      descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(),
      descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(
    uint32_t size, D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = xe::align(size, kScratchBufferSizeIncrement);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      buffer_desc, size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault,
          provider.GetHeapFlagCreateNotZeroed(), &buffer_desc, state, nullptr,
          IID_PPV_ARGS(&buffer)))) {
    XELOGE("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  buffer->SetName(L"Scratch GPU Buffer");
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                         scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(
    ID3D12Resource* buffer, D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(
    ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
#if XE_ARCH_AMD64 == 1
  __m128 zero_register = _mm_setzero_ps();
  __m128 ff_viewport_low4 = _mm_loadu_ps(&ff_viewport_.TopLeftX);
  __m128 ff_viewport_high2 =
      _mm_loadl_pi(zero_register, (const __m64*)&ff_viewport_.MinDepth);

  __m128 viewport_low4 = _mm_loadu_ps(&viewport.TopLeftX);
  __m128 viewport_high2 =
      _mm_loadl_pi(zero_register, (const __m64*)&viewport.MinDepth);

  __m128 first_four_cmp = _mm_cmpeq_ps(ff_viewport_low4, viewport_low4);
  __m128 last_two_cmp = _mm_cmpeq_ps(ff_viewport_high2, viewport_high2);

  __m128 combined_condition = _mm_and_ps(first_four_cmp, last_two_cmp);

  int movmask = _mm_movemask_ps(combined_condition);

  XE_UNLIKELY_IF(ff_viewport_update_needed_ || movmask != 0b1111)
#else
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (XE_UNLIKELY(ff_viewport_update_needed_))
#endif
  {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
#if XE_ARCH_AMD64 == 1
  // vtune suggested that this and SetViewport be vectorized, high retiring
  // figure
  __m128i scissor_m128 = _mm_loadu_si128((const __m128i*)&scissor_rect);
  __m128i ff_scissor_m128 = _mm_loadu_si128((const __m128i*)&ff_scissor_);
  __m128i comparison_result = _mm_cmpeq_epi32(scissor_m128, ff_scissor_m128);
  if (ff_scissor_update_needed_ ||
      _mm_movemask_epi8(comparison_result) != 0xFFFF)
#else
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;

  if (ff_scissor_update_needed_)
#endif
  {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(
    D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  // Rasterizer-ordered views are a feature very rarely used as of 2020 and
  // that faces adoption complications (outside of Direct3D - on Vulkan - at
  // least), but crucial to Xenia - raise awareness of its usage.
  // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
  // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
  //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
  //  feature" - oscarbg in that issue.
  switch (render_target_cache_->GetPath()) {
    case RenderTargetCache::Path::kHostRenderTargets:
      suffix << " - RTV/DSV";
      break;
    case RenderTargetCache::Path::kPixelShaderInterlock:
      suffix << " - ROV";
      break;
    default:
      break;
  }
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  // Check if debug markers should be enabled (CVAR).
  UpdateDebugMarkersEnabled();
  if (debug_markers_enabled_) {
    XELOGI("GPU debug markers enabled for PIX/RenderDoc/debug tools");
  }

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  completion_timeline_ = ui::d3d12::D3D12GPUCompletionTimeline::Create(device);
  queue_operations_since_submission_completion_timeline_ =
      ui::d3d12::D3D12GPUCompletionTimeline::Create(device);
  if (!completion_timeline_ ||
      !queue_operations_since_submission_completion_timeline_) {
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)))) {
    XELOGE("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       command_allocator, nullptr,
                                       IID_PPV_ARGS(&command_list_)))) {
    XELOGE("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_2_));

  bindless_resources_used_ =
      cvars::d3d12_bindless &&
      provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;
  // The guest shader path is Mesa spirv_to_dxil only, which is bindless (SM 6.6
  // ResourceDescriptorHeap). There is no bindful guest shader path anymore, so
  // a device without bindless support (resource binding tier 1, or
  // d3d12_bindless disabled) cannot use the D3D12 backend - use the Vulkan
  // backend instead.
  if (!bindless_resources_used_) {
    XELOGE(
        "The D3D12 backend requires bindless resources (resource binding tier "
        "2 "
        "and d3d12_bindless enabled). Use the Vulkan backend on this device.");
    return false;
  }

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                                 draw_resolution_scale_y);

  bool has_tiled_resources =
      provider.GetTiledResourcesTier() >= D3D12_TILED_RESOURCES_TIER_1;
  if (!TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, has_tiled_resources,
          provider.GetVirtualAddressBitsPerResource())) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    XELOGW(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ =
      std::make_unique<D3D12SharedMemory>(*this, *memory_, trace_writer_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Read-watch consumption tracking for resolves.
  InitResolveReadWatch();
  resolve_read_callback_ = memory_->RegisterPhysicalMemoryReadCallback(
      ResolveReadCallbackThunk, this);

  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize()) {
    XELOGE("Failed to initialize the render target cache");
    return false;
  }

  // Hybrid RTV queries count pre-test coverage in the pixel shader and clear
  // their counter slot with WriteBufferImmediate, so they need CommandList2.
  zpd_hybrid_supported_ = cvars::occlusion_query_full_counters &&
                          command_list_2_ &&
                          render_target_cache_->GetPath() ==
                              RenderTargetCache::Path::kHostRenderTargets;

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider,
      std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
               sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(
            &view_bindless_heap_desc, IID_PPV_ARGS(&view_bindless_heap_)))) {
      XELOGE("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ =
        view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ =
        view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags =
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(
            &sampler_bindless_heap_desc,
            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      XELOGE("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ =
        std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
            device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ =
        std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
            device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  {
    // Fixed root signature for the spirv_to_dxil guest path. CBVs in space1,
    // shared memory in space0 (same registers as the other paths), runtime data
    // in space31, matching the Mesa DXIL binding layout.
    D3D12_ROOT_PARAMETER root_parameters_mesa[kRootParameter_Mesa_Count] = {};
    auto set_mesa_cbv = [&](uint32_t index, uint32_t shader_register,
                            uint32_t register_space) {
      auto& parameter = root_parameters_mesa[index];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister = shader_register;
      parameter.Descriptor.RegisterSpace = register_space;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    set_mesa_cbv(kRootParameter_Mesa_SystemConstants, 0, 1);
    set_mesa_cbv(kRootParameter_Mesa_FloatConstantsVertex, 1, 1);
    set_mesa_cbv(kRootParameter_Mesa_FloatConstantsPixel, 2, 1);
    set_mesa_cbv(kRootParameter_Mesa_BoolLoopConstants, 3, 1);
    set_mesa_cbv(kRootParameter_Mesa_FetchConstants, 4, 1);
    set_mesa_cbv(kRootParameter_Mesa_RuntimeData, 0, 31);
    auto set_mesa_srv = [&](uint32_t index, uint32_t shader_register,
                            uint32_t register_space) {
      auto& parameter = root_parameters_mesa[index];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      parameter.Descriptor.ShaderRegister = shader_register;
      parameter.Descriptor.RegisterSpace = register_space;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    set_mesa_srv(kRootParameter_Mesa_VertexTextureIndices, 2, 0);
    set_mesa_srv(kRootParameter_Mesa_PixelTextureIndices, 3, 0);
    // Unbounded ranges covering the (heap-accessed) texture/sampler
    // declarations the bindless lowering leaves behind, so D3D12 accepts the
    // root signature.
    D3D12_DESCRIPTOR_RANGE mesa_declaration_ranges[4] = {};
    auto set_mesa_declaration_range =
        [&](uint32_t index, D3D12_DESCRIPTOR_RANGE_TYPE range_type,
            uint32_t register_space, D3D12_DESCRIPTOR_RANGE& range) {
          range.RangeType = range_type;
          range.NumDescriptors = UINT_MAX;  // Unbounded.
          range.BaseShaderRegister = 0;
          range.RegisterSpace = register_space;
          range.OffsetInDescriptorsFromTableStart = 0;
          auto& parameter = root_parameters_mesa[index];
          parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
          parameter.DescriptorTable.NumDescriptorRanges = 1;
          parameter.DescriptorTable.pDescriptorRanges = &range;
          parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        };
    set_mesa_declaration_range(kRootParameter_Mesa_VertexTextureRange,
                               D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2,
                               mesa_declaration_ranges[0]);
    set_mesa_declaration_range(kRootParameter_Mesa_PixelTextureRange,
                               D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3,
                               mesa_declaration_ranges[1]);
    set_mesa_declaration_range(kRootParameter_Mesa_VertexSamplerRange,
                               D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2,
                               mesa_declaration_ranges[2]);
    set_mesa_declaration_range(kRootParameter_Mesa_PixelSamplerRange,
                               D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 3,
                               mesa_declaration_ranges[3]);
    D3D12_DESCRIPTOR_RANGE mesa_shared_memory_ranges[2] = {};
    {
      auto& parameter = root_parameters_mesa[kRootParameter_Mesa_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(xe::countof(mesa_shared_memory_ranges));
      parameter.DescriptorTable.pDescriptorRanges = mesa_shared_memory_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = mesa_shared_memory_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = kMesaRegister_SharedMemory;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = mesa_shared_memory_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = kMesaRegister_SharedMemory;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // ZPD counter (u1) and EDRAM (u2) raw UAVs. Single descriptor tables
    // pointed into the bindless system view heap at draw time
    // (UpdateBindingsMesa), mirroring the shared memory table. The Mesa DXIL
    // places the SPIR-V set 0 bindings 1 and 2 at u1/u2 space0.
    D3D12_DESCRIPTOR_RANGE mesa_edram_range = {};
    {
      mesa_edram_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      mesa_edram_range.NumDescriptors = 1;
      mesa_edram_range.BaseShaderRegister = kMesaRegister_Edram;
      mesa_edram_range.RegisterSpace = 0;
      mesa_edram_range.OffsetInDescriptorsFromTableStart = 0;
      auto& parameter = root_parameters_mesa[kRootParameter_Mesa_Edram];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &mesa_edram_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_DESCRIPTOR_RANGE mesa_zpd_counter_range = {};
    {
      mesa_zpd_counter_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      mesa_zpd_counter_range.NumDescriptors = 1;
      mesa_zpd_counter_range.BaseShaderRegister = kMesaRegister_ZpdCounter;
      mesa_zpd_counter_range.RegisterSpace = 0;
      mesa_zpd_counter_range.OffsetInDescriptorsFromTableStart = 0;
      auto& parameter = root_parameters_mesa[kRootParameter_Mesa_ZpdCounter];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &mesa_zpd_counter_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC root_signature_mesa_desc = {};
    root_signature_mesa_desc.NumParameters = kRootParameter_Mesa_Count;
    root_signature_mesa_desc.pParameters = root_parameters_mesa;
    // The bindless lowering accesses textures via ResourceDescriptorHeap and
    // samplers via SamplerDescriptorHeap, so both heaps must be directly
    // indexable from the shader.
    root_signature_mesa_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    root_signature_mesa_ = ui::d3d12::util::CreateRootSignature(
        GetD3D12Provider(), root_signature_mesa_desc);
    if (!root_signature_mesa_) {
      XELOGE("Failed to create the spirv_to_dxil guest root signature");
      return false;
    }
  }

  primitive_processor_ = std::make_unique<D3D12PrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ = D3D12TextureCache::Create(
      *register_file_, *shared_memory_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    XELOGE("Failed to initialize the texture cache");
    return false;
  }

  // Fallback for query segment normalization when no draw pinned a scale.
  zpd_draw_resolution_scale_x_ = draw_resolution_scale_x;
  zpd_draw_resolution_scale_y_ = draw_resolution_scale_y;

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(),
      bindless_resources_used_, zpd_hybrid_supported_);
  if (!pipeline_cache_->Initialize()) {
    XELOGE("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed =
      provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(
      gamma_ramp_buffer_desc, (256 + 128 * 3) * 4, D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault, heap_flag_create_not_zeroed,
          &gamma_ramp_buffer_desc, gamma_ramp_buffer_state_, nullptr,
          IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    XELOGE("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;

  if (!GetD3D12Provider().CreateUploadResource(
          heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ,
          IID_PPV_ARGS(&gamma_ramp_upload_buffer_))) {
    XELOGE("Failed to create the gamma ramp upload buffer");
    return false;
  }

  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr,
          reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    XELOGE("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType =
      D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(
            ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType =
      D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart =
      0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType =
      D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges =
        1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters =
      UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider,
                                           apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    XELOGE("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(
          device, shaders::apply_gamma_table_cs,
          sizeof(shaders::apply_gamma_table_cs),
          apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    XELOGE(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(
          device, shaders::apply_gamma_table_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
          apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    XELOGE(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(
          device, shaders::apply_gamma_pwl_cs,
          sizeof(shaders::apply_gamma_pwl_cs),
          apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    XELOGE("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(
          device, shaders::apply_gamma_pwl_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
          apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    XELOGE(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    XELOGE("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::fxaa_cs,
                                             sizeof(shaders::fxaa_cs),
                                             fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    XELOGE("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::fxaa_extreme_cs,
                                             sizeof(shaders::fxaa_extreme_cs),
                                             fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    XELOGE("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  // Resolve downscale compute shader for scaled resolution readback.
  // Root parameter 0: Constants (scale_x, scale_y, pixel_size_log2, tile_count)
  // Root parameter 1: Source SRV (ByteAddressBuffer)
  // Root parameter 2: Destination UAV (RWByteAddressBuffer)
  {
    D3D12_ROOT_PARAMETER resolve_downscale_root_parameters[UINT(
        ResolveDownscaleRootParameter::kCount)];
    // Parameter 0: Constants.
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kConstants)]
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kConstants)]
            .Constants.ShaderRegister = 0;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kConstants)]
            .Constants.RegisterSpace = 0;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kConstants)]
            .Constants.Num32BitValues =
        sizeof(ResolveDownscaleConstants) / sizeof(uint32_t);
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kConstants)]
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Parameter 1: Source SRV.
    D3D12_DESCRIPTOR_RANGE resolve_downscale_srv_range;
    resolve_downscale_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    resolve_downscale_srv_range.NumDescriptors = 1;
    resolve_downscale_srv_range.BaseShaderRegister = 0;
    resolve_downscale_srv_range.RegisterSpace = 0;
    resolve_downscale_srv_range.OffsetInDescriptorsFromTableStart = 0;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kSource)]
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kSource)]
            .DescriptorTable.NumDescriptorRanges = 1;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kSource)]
            .DescriptorTable.pDescriptorRanges = &resolve_downscale_srv_range;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kSource)]
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Parameter 2: Destination UAV.
    D3D12_DESCRIPTOR_RANGE resolve_downscale_uav_range;
    resolve_downscale_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    resolve_downscale_uav_range.NumDescriptors = 1;
    resolve_downscale_uav_range.BaseShaderRegister = 0;
    resolve_downscale_uav_range.RegisterSpace = 0;
    resolve_downscale_uav_range.OffsetInDescriptorsFromTableStart = 0;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kDestination)]
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kDestination)]
            .DescriptorTable.NumDescriptorRanges = 1;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kDestination)]
            .DescriptorTable.pDescriptorRanges = &resolve_downscale_uav_range;
    resolve_downscale_root_parameters
        [UINT(ResolveDownscaleRootParameter::kDestination)]
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC resolve_downscale_root_signature_desc;
    resolve_downscale_root_signature_desc.NumParameters =
        UINT(ResolveDownscaleRootParameter::kCount);
    resolve_downscale_root_signature_desc.pParameters =
        resolve_downscale_root_parameters;
    resolve_downscale_root_signature_desc.NumStaticSamplers = 0;
    resolve_downscale_root_signature_desc.pStaticSamplers = nullptr;
    resolve_downscale_root_signature_desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_NONE;
    *(resolve_downscale_root_signature_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateRootSignature(
            provider, resolve_downscale_root_signature_desc);
    if (!resolve_downscale_root_signature_) {
      XELOGE("Failed to create the resolve downscale root signature");
      return false;
    }
    *(resolve_downscale_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(
            device, shaders::resolve_downscale_cs,
            sizeof(shaders::resolve_downscale_cs),
            resolve_downscale_root_signature_.Get());
    if (!resolve_downscale_pipeline_) {
      XELOGE("Failed to create the resolve downscale compute pipeline");
      return false;
    }
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping =
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // Device SRV + UAV pair for memexport draws that also read shared memory
    // (guest vertex fetch via t0). Both address the device buffer.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kSharedMemoryRawSRVForRW)));
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kSharedMemoryRawUAVForRW)));
    // Host buffer pairs for two-buffer memexport routing, only when it exists.
    // The read pair is [host SRV, null UAV], the write pair [null SRV, host
    // UAV], matching the device layout above.
    if (shared_memory_->GetHostBuffer()) {
      ui::d3d12::util::CreateBufferRawSRV(
          device,
          provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_,
              uint32_t(SystemBindlessView::kSharedMemoryHostRawSRV)),
          shared_memory_->GetHostBuffer(), SharedMemory::kBufferSize);
      ui::d3d12::util::CreateBufferRawUAV(
          device,
          provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_,
              uint32_t(SystemBindlessView::kSharedMemoryHostNullRawUAV)),
          nullptr, 0);
      ui::d3d12::util::CreateBufferRawSRV(
          device,
          provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_,
              uint32_t(SystemBindlessView::kSharedMemoryHostNullRawSRV)),
          nullptr, 0);
      ui::d3d12::util::CreateBufferRawUAV(
          device,
          provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_,
              uint32_t(SystemBindlessView::kSharedMemoryHostRawUAV)),
          shared_memory_->GetHostBuffer(), SharedMemory::kBufferSize);
      // Host SRV + UAV pair for memexport draws that also read shared memory
      // (guest vertex fetch via t0). Both address the host buffer.
      ui::d3d12::util::CreateBufferRawSRV(
          device,
          provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_,
              uint32_t(SystemBindlessView::kSharedMemoryHostRawSRVForRW)),
          shared_memory_->GetHostBuffer(), SharedMemory::kBufferSize);
      ui::d3d12::util::CreateBufferRawUAV(
          device,
          provider.OffsetViewDescriptor(
              view_bindless_heap_cpu_start_,
              uint32_t(SystemBindlessView::kSharedMemoryHostRawUAVForRW)),
          shared_memory_->GetHostBuffer(), SharedMemory::kBufferSize);
    }
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kZpdCounterRawUAV.
    WriteZPDCounterRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kZpdCounterRawUAV)));
    // kGammaRampTableSRV.
    WriteGammaRampSRV(false,
                      provider.OffsetViewDescriptor(
                          view_bindless_heap_cpu_start_,
                          uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(true,
                      provider.OffsetViewDescriptor(
                          view_bindless_heap_cpu_start_,
                          uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  // Initialize the ZPD occlusion query pool and resources.
  zpd_host_query_pool_ = std::make_unique<D3D12ZPDQueryPool>();
  EnsureZPDQueryResources();

  pix_capture_requested_.store(false, std::memory_order_relaxed);
  pix_capturing_ = false;

  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();

  ResetMemexportPages();
  memexport_staged_.clear();
  ResetResolveReadWatch();
  DestroyResolveFaultCopy();

  ShutdownZPDQueryResources();
  zpd_host_query_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;

  // Before the deletion list is drained, staging buffers are freed through it.
  ClearReadbackStagingBuffers();

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();

  resolve_downscale_buffer_.Reset();
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_pipeline_.Reset();
  resolve_downscale_root_signature_.Reset();

  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_mesa_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed :
         sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_2_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  queue_operations_since_submission_completion_timeline_.reset();

  submission_open_ = false;
  completion_timeline_.reset();

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteRegisterForceinline(uint32_t index,
                                                     uint32_t value) {
  // Parallel range check: is index within any of these GPU register ranges?
  // Each range maps to a bit in movmask (by byte pair position).
  register_file_->values[index] = value;
#if XE_ARCH_AMD64
  __m128i to_rangecheck = _mm_set1_epi16(static_cast<short>(index));
  __m128i lower_bounds = _mm_setr_epi16(
      XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 - 1,
      XE_GPU_REG_SHADER_CONSTANT_000_X - 1,
      XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 - 1, XE_GPU_REG_SCRATCH_REG0 - 1,
      XE_GPU_REG_COHER_STATUS_HOST - 1, XE_GPU_REG_DC_LUT_RW_INDEX - 1, 0, 0);
  __m128i upper_bounds = _mm_setr_epi16(
      XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1,
      XE_GPU_REG_SHADER_CONSTANT_511_W + 1,
      XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1, XE_GPU_REG_SCRATCH_REG7 + 1,
      XE_GPU_REG_COHER_STATUS_HOST + 1, XE_GPU_REG_DC_LUT_30_COLOR + 1, 0, 0);
  __m128i is_above_lower = _mm_cmpgt_epi16(to_rangecheck, lower_bounds);
  __m128i is_below_upper = _mm_cmplt_epi16(to_rangecheck, upper_bounds);
  __m128i is_within_range = _mm_and_si128(is_above_lower, is_below_upper);
  uint32_t movmask = static_cast<uint32_t>(_mm_movemask_epi8(is_within_range));
#else
  auto in_range = [index](uint32_t lo, uint32_t hi) -> uint32_t {
    return (index > lo && index < hi) ? 0x3 : 0;
  };
  uint32_t movmask = 0;
  movmask |= in_range(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 - 1,
                      XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1)
             << 0;
  movmask |= in_range(XE_GPU_REG_SHADER_CONSTANT_000_X - 1,
                      XE_GPU_REG_SHADER_CONSTANT_511_W + 1)
             << 2;
  movmask |= in_range(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 - 1,
                      XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1)
             << 4;
  movmask |= in_range(XE_GPU_REG_SCRATCH_REG0 - 1, XE_GPU_REG_SCRATCH_REG7 + 1)
             << 6;
  movmask |= in_range(XE_GPU_REG_COHER_STATUS_HOST - 1,
                      XE_GPU_REG_COHER_STATUS_HOST + 1)
             << 8;
  movmask |=
      in_range(XE_GPU_REG_DC_LUT_RW_INDEX - 1, XE_GPU_REG_DC_LUT_30_COLOR + 1)
      << 10;
#endif

  if (movmask) {
    if (movmask & (1 << 3)) {
      if (frame_open_) {
        uint32_t float_constant_index =
            (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
        uint64_t float_constant_mask = 1ULL << float_constant_index;

        if (float_constant_index >= 256) {
          float_constant_index =
              static_cast<unsigned char>(float_constant_index);
          if (current_float_constant_map_pixel_[float_constant_index >> 6] &
              float_constant_mask) {  // take advantage of x86
                                      // modulus shift
            cbuffer_binding_float_pixel_.up_to_date = false;
          }
        } else {
          if (current_float_constant_map_vertex_[float_constant_index >> 6] &
              float_constant_mask) {
            cbuffer_binding_float_vertex_.up_to_date = false;
          }
        }
      }
    } else if (movmask & (1 << 5)) {
      cbuffer_binding_bool_loop_.up_to_date = false;
    } else if (movmask & (1 << 1)) {
      cbuffer_binding_fetch_.up_to_date = false;

      texture_cache_->TextureFetchConstantWritten(
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  } else {
    _ReadWriteBarrier();
    return;
  }
}
// todo: bit-pack the bools and use bitarith to reduce branches
void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  LogRegisterSet(index, value);
  WriteRegisterForceinline(index, value);
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0, 0xFFFF>(start_index, base,
                                                      num_registers);
}

void D3D12CommandProcessor::WriteALURangeFromRing(xe::RingBuffer* ring,
                                                  uint32_t base,
                                                  uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<
      XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0>(
      ring, base + XE_GPU_REG_SHADER_CONSTANT_000_X, num_times);
}

void D3D12CommandProcessor::WriteFetchRangeFromRing(xe::RingBuffer* ring,
                                                    uint32_t base,
                                                    uint32_t num_times) {
  WriteRegisterRangeFromRing_WithKnownBound<0x4800, 0x5002>(ring, base + 0x4800,
                                                            num_times);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteBoolRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  // D3D12CommandProcessor::WriteRegisterRangeFromRing(ring, base + 0x4900,
  //                                                   num_times);

  WriteRegisterRangeFromRing_WithKnownBound<0x4900, 0x5002>(ring, base + 0x4900,
                                                            num_times);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteLoopRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  // D3D12CommandProcessor::WriteRegisterRangeFromRing(ring, base + 0x4908,
  //                                                   num_times);

  WriteRegisterRangeFromRing_WithKnownBound<0x4908, 0x5002>(ring, base + 0x4908,
                                                            num_times);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteREGISTERSRangeFromRing(xe::RingBuffer* ring,
                                                        uint32_t base,
                                                        uint32_t num_times) {
  // D3D12CommandProcessor::WriteRegisterRangeFromRing(ring, base + 0x2000,
  //                                                  num_times);

  WriteRegisterRangeFromRing_WithKnownBound<0x2000, 0x2000 + 0x800>(
      ring, base + 0x2000, num_times);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteALURangeFromMem(uint32_t start_index,
                                                 uint32_t* base,
                                                 uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<
      XE_GPU_REG_SHADER_CONSTANT_000_X, XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0>(
      start_index + 0x4000, base, num_registers);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index,
                                                   uint32_t* base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x4800, 0x5002>(start_index + 0x4800,
                                                           base, num_registers);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x4900, 0x5002>(start_index + 0x4900,
                                                           base, num_registers);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x4908, 0x5002>(start_index + 0x4908,
                                                           base, num_registers);
}

XE_FORCEINLINE
void D3D12CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index,
                                                       uint32_t* base,
                                                       uint32_t num_registers) {
  WriteRegisterRangeFromMem_WithKnownBound<0x2000, 0x2000 + 0x800>(
      start_index + 0x2000, base, num_registers);
}
/*
wraparound rarely happens, so its best to hoist this out of
writeregisterrangefromring, and structure the two functions so that this can be
tail called
*/
XE_NOINLINE
void D3D12CommandProcessor::WriteRegisterRangeFromRing_WraparoundCase(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  // we already brought it into L2 earlier
  RingBuffer::ReadRange range =
      ring->BeginPrefetchedRead<swcache::PrefetchTag::Level1>(num_registers *
                                                              sizeof(uint32_t));

  uint32_t num_regs_firstrange =
      static_cast<uint32_t>(range.first_length / sizeof(uint32_t));

  D3D12CommandProcessor::WriteRegistersFromMem(
      base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      num_regs_firstrange);

  D3D12CommandProcessor::WriteRegistersFromMem(
      base + num_regs_firstrange,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
      num_registers - num_regs_firstrange);

  ring->EndRead(range);
}
void D3D12CommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                       uint32_t base,
                                                       uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));

  XE_LIKELY_IF(!range.second) {
    WriteRegistersFromMem(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);

    ring->EndRead(range);
  }
  else {
    return WriteRegisterRangeFromRing_WraparoundCase(ring, base, num_registers);
  }
}

template <uint32_t register_lower_bound, uint32_t register_upper_bound>
constexpr bool bounds_may_have_reg(uint32_t reg) {
  return reg >= register_lower_bound && reg < register_upper_bound;
}

template <uint32_t register_lower_bound, uint32_t register_upper_bound>
constexpr bool bounds_may_have_bounds(uint32_t reg, uint32_t last_reg) {
  return bounds_may_have_reg<register_lower_bound, register_upper_bound>(reg) ||
         bounds_may_have_reg<register_lower_bound, register_upper_bound>(
             last_reg);
}
XE_FORCEINLINE
void D3D12CommandProcessor::WriteShaderConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (frame_open_) {
    bool cbuffer_pixel_uptodate = cbuffer_binding_float_pixel_.up_to_date;
    bool cbuffer_vertex_uptodate = cbuffer_binding_float_vertex_.up_to_date;
    if (cbuffer_pixel_uptodate || cbuffer_vertex_uptodate) {
      // super naive, could just do some bit magic and interval checking,
      // but we just need this hoisted out of the copy so we do a bulk copy
      // because its the actual load/swap/store we're getting murdered by
      // this precheck followed by copy_and_swap_32_unaligned reduced the cpu
      // usage from packettype0/writeregistersfrommem from 10-11% of cpu time
      // spent on xenia to like 1%
      // chrispy: todo, this can be reduced even further, should be split into
      // two loops and should skip whole words, this could net us even bigger
      // gains
      uint32_t map_index = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) / 4;
      uint32_t end_map_index =
          (start_index + num_registers - XE_GPU_REG_SHADER_CONSTANT_000_X) / 4;

      if (map_index < 256 && cbuffer_vertex_uptodate) {
        for (; map_index < end_map_index; ++map_index) {
          if (current_float_constant_map_vertex_[map_index >> 6] &
              (1ull << map_index)) {
            cbuffer_vertex_uptodate = false;
            break;
          }
        }
      }
      if (end_map_index > 256 && cbuffer_pixel_uptodate) {
        for (; map_index < end_map_index; ++map_index) {
          uint32_t float_constant_index = map_index;
          float_constant_index -= 256;
          if (current_float_constant_map_pixel_[float_constant_index >> 6] &
              (1ull << float_constant_index)) {
            cbuffer_pixel_uptodate = false;
            break;
          }
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = cbuffer_pixel_uptodate;
    cbuffer_binding_float_vertex_.up_to_date = cbuffer_vertex_uptodate;
  }

  // maybe use non-temporal copy if possible...
  copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                             num_registers);
}
XE_FORCEINLINE
void D3D12CommandProcessor::WriteBoolLoopFromMem(uint32_t start_index,
                                                 uint32_t* base,
                                                 uint32_t num_registers) {
  cbuffer_binding_bool_loop_.up_to_date = false;
  copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                             num_registers);
}
XE_FORCEINLINE
void D3D12CommandProcessor::WriteFetchFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  cbuffer_binding_fetch_.up_to_date = false;

  uint32_t first_fetch =
      ((start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);
  uint32_t last_fetch =  // i think last_fetch should be inclusive if its modulo
                         // is nz...
      (((start_index + num_registers) - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
       6);
  texture_cache_->TextureFetchConstantsWritten(first_fetch, last_fetch);

  copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                             num_registers);
}

void D3D12CommandProcessor::WritePossiblySpecialRegistersFromMem(
    uint32_t start_index, uint32_t* base, uint32_t numregs) {
  uint32_t end = numregs + start_index;
  for (uint32_t index = start_index; index < end; ++index, ++base) {
    uint32_t value = xe::load_and_swap<uint32_t>(base);

    register_file_->values[index] = value;

    unsigned expr = 0;

    expr |= (index - XE_GPU_REG_SCRATCH_REG0 < 8);

    expr |= (index == XE_GPU_REG_COHER_STATUS_HOST);

    expr |= ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
             (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));

    if (expr == 0) {
    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  }
}
template <uint32_t register_lower_bound, uint32_t register_upper_bound>
XE_FORCEINLINE void
D3D12CommandProcessor::WriteRegisterRangeFromMem_WithKnownBound(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  uint32_t end = start_index + num_registers;
  LogRegisterSets(start_index, base, num_registers);
  uint32_t current_index = start_index;

  auto get_end_before_qty = [&end, current_index](uint32_t regnum) {
    return std::min<uint32_t>(regnum, end) - current_index;
  };
#define REGULAR_WRITE_CALLBACK(s, e, i, b, n) \
  copy_and_swap_32_unaligned(&register_file_->values[i], b, n)
#define WRITE_FETCH_CONSTANTS_CALLBACK(str, er, ind, b, n) \
  WriteFetchFromMem(ind, b, n)
#define SPECIAL_REG_RANGE_CALLBACK(str, edr, ind, bs, n) \
  WritePossiblySpecialRegistersFromMem(ind, bs, n)
#define WRITE_SHADER_CONSTANTS_CALLBACK(start_range, end_range, index, base, \
                                        n)                                   \
  WriteShaderConstantsFromMem(index, base, n)
#define WRITE_BOOL_LOOP_CALLBACK(s, e, i, b, n) WriteBoolLoopFromMem(i, b, n)

#define DO_A_RANGE(start_range, end_range, cb)                       \
  if constexpr (start_range >= register_lower_bound ||               \
                end_range > register_lower_bound) {                  \
    if (current_index < (end_range)) {                               \
      uint32_t ntowrite = get_end_before_qty(end_range);             \
      cb((start_range), (end_range), current_index, base, ntowrite); \
      current_index += ntowrite;                                     \
      base += ntowrite;                                              \
    }                                                                \
    if (current_index >= end) {                                      \
      return;                                                        \
    }                                                                \
  }

#define REFRESH_MSVC_RANGE()                              \
  XE_MSVC_ASSUME(current_index >= register_lower_bound && \
                 current_index < register_upper_bound)

  REFRESH_MSVC_RANGE();

  DO_A_RANGE(0, XE_GPU_REG_SCRATCH_REG0, REGULAR_WRITE_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SCRATCH_REG0, XE_GPU_REG_DC_LUT_30_COLOR + 1,
             SPECIAL_REG_RANGE_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_DC_LUT_30_COLOR + 1, XE_GPU_REG_SHADER_CONSTANT_000_X,
             REGULAR_WRITE_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_000_X,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
             WRITE_SHADER_CONSTANTS_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
             XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1,
             WRITE_FETCH_CONSTANTS_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031,
             XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1, WRITE_BOOL_LOOP_CALLBACK);
  REFRESH_MSVC_RANGE();
  DO_A_RANGE(XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1, 65536,
             REGULAR_WRITE_CALLBACK);
}
template <uint32_t register_lower_bound, uint32_t register_upper_bound>
XE_FORCEINLINE void
D3D12CommandProcessor::WriteRegisterRangeFromRing_WithKnownBound(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));

  XE_LIKELY_IF(!range.second) {
    WriteRegisterRangeFromMem_WithKnownBound<register_lower_bound,
                                             register_upper_bound>(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);

    ring->EndRead(range);
  }
  else {
    return WriteRegisterRangeFromRing_WraparoundCase(ring, base, num_registers);
  }
}
XE_NOINLINE
void D3D12CommandProcessor::WriteOneRegisterFromRing(uint32_t base,
                                                     uint32_t num_times) {
  auto read = reader_.BeginPrefetchedRead<swcache::PrefetchTag::Level1>(
      num_times * sizeof(uint32_t));

  uint32_t first_length = read.first_length / sizeof(uint32_t);

  for (uint32_t i = 0; i < first_length; ++i) {
    D3D12CommandProcessor::WriteRegister(
        base, xe::load_and_swap<uint32_t>(read.first + (sizeof(uint32_t) * i)));
  }

  XE_UNLIKELY_IF(read.second) {
    uint32_t second_length = read.second_length / sizeof(uint32_t);

    for (uint32_t i = 0; i < second_length; ++i) {
      D3D12CommandProcessor::WriteRegister(
          base,
          xe::load_and_swap<uint32_t>(read.second + (sizeof(uint32_t) * i)));
    }
  }
  reader_.EndRead(read);
}
void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  EndZPDFrame();

  // Before the presenter check, so the paths that return early end the frame
  // too.
  NoteResolveFrame();

  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    return;
  }

  // Obtain the actual front buffer size to pass to RefreshGuestOutput,
  // resolution-scaled if it's a resolve destination, or not otherwise.
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  ID3D12Resource* swap_texture_resource = texture_cache_->RequestSwapTexture(
      swap_texture_srv_desc, frontbuffer_format);
  if (!swap_texture_resource) {
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();

  auto aspect = graphics_system_->GetScaledAspectRatio();

  presenter->RefreshGuestOutput(
      uint32_t(swap_texture_desc.Width), uint32_t(swap_texture_desc.Height),
      aspect.first, aspect.second,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource,
       &swap_texture_desc](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc =
                fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != swap_texture_desc.Width ||
                fxaa_source_texture_desc.Height != swap_texture_desc.Height) {
              if (GetCompletedSubmission() < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(
                    fxaa_source_texture_submission_,
                    fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension =
                D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = swap_texture_desc.Width;
            fxaa_source_texture_desc.Height = swap_texture_desc.Height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags =
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesDefault,
                    provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&fxaa_source_texture_)))) {
              XELOGE("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format ==
                xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes =
              (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little &&
              use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                    gamma_ramp_upload_buffer_mapping_ +
                    gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base =
                  gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta =
                  gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(
                gamma_ramp_upload_buffer_mapping_ +
                    gamma_ramp_upload_offset_bytes,
                use_pwl_gamma_ramp
                    ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                    : static_cast<const void*>(gamma_ramp_256_entry_table()),
                gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(),
                                gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          InsertDebugMarker("Gamma Ramp Upload: %u bytes",
                            gamma_ramp_size_bytes);
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes,
              gamma_ramp_upload_buffer_.Get(), gamma_ramp_upload_offset_bytes,
              gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::d3d12::util::DescriptorCpuGpuHandlePair
            apply_gamma_descriptor_gamma_ramp;
        if (!RequestOneUseSingleViewDescriptors(
                bindless_resources_used_ ? 2 : 3, apply_gamma_descriptors)) {
          return false;
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp,
                            apply_gamma_descriptor_gamma_ramp.first);
        }

        ID3D12Resource* guest_output_resource =
            static_cast<
                ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(
                context)
                .resource_uav_capable();

        if (use_fxaa) {
          fxaa_source_texture_submission_ = GetCurrentSubmission();
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get() : guest_output_resource;
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            use_fxaa ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(
            context)
            .resource_uav_capable();
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat
                     : ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr,
                                          &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource,
                                         &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(),
                              gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        PushDebugMarker("Apply Gamma Ramp: %s",
                        use_pwl_gamma_ramp ? "PWL" : "256-entry table");

        deferred_command_list_.D3DSetComputeRootSignature(
            apply_gamma_root_signature_.Get());
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = uint32_t(swap_texture_desc.Width);
        apply_gamma_constants.size[1] = uint32_t(swap_texture_desc.Height);
        deferred_command_list_.D3DSetComputeRoot32BitConstants(
            UINT(ApplyGammaRootParameter::kConstants),
            sizeof(apply_gamma_constants) / sizeof(uint32_t),
            &apply_gamma_constants, 0);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kDestination),
            apply_gamma_descriptors[0].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kSource),
            apply_gamma_descriptors[1].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kRamp),
            apply_gamma_descriptor_gamma_ramp.second);
        ID3D12PipelineState* apply_gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          apply_gamma_pipeline = use_fxaa
                                     ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                     : apply_gamma_pwl_pipeline_.Get();
        } else {
          apply_gamma_pipeline =
              use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                       : apply_gamma_table_pipeline_.Get();
        }
        SetExternalPipeline(apply_gamma_pipeline);
        SubmitBarriers();
        uint32_t group_count_x = (uint32_t(swap_texture_desc.Width) + 15) / 16;
        uint32_t group_count_y = (uint32_t(swap_texture_desc.Height) + 7) / 8;
        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        PopDebugMarker();

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(
                  uint32_t(xe::countof(fxaa_descriptors)), fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(
                guest_output_resource,
                ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource,
                                                   apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(
                guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(
                guest_output_resource,
                ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(
                fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = uint32_t(swap_texture_desc.Width);
            fxaa_constants.size[1] = uint32_t(swap_texture_desc.Height);
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants),
                sizeof(fxaa_constants) / sizeof(uint32_t), &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format =
                ui::d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr,
                                              &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination),
                fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(),
                                             &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(
                guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        SubmitBarriers();
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (cvars::submit_on_primary_buffer_end && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                          const uint32_t* host_address,
                                          uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool D3D12CommandProcessor::EnsureMemexportRangeInDeviceBuffer(
    uint32_t base_bytes, uint32_t size_bytes) {
  if (!cvars::memexport_enable || shared_memory_->GetHostBuffer() == nullptr ||
      !size_bytes || base_bytes >= SharedMemory::kBufferSize) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  if (!IsMemexportRange(base_bytes, size_bytes)) {
    return false;
  }
  // Transition the host buffer to a copy source (ordering the memexport writes,
  // which may have run several draws ago, before the read) and the device
  // buffer to a copy destination, then copy just this range across. The
  // following texture-load UseForReading transitions the device buffer to a
  // read state, ordering the copy before the load.
  shared_memory_->UseHostAsCopySource();
  shared_memory_->UseAsCopyDestination();
  SubmitBarriers();
  deferred_command_list_.D3DCopyBufferRegion(
      shared_memory_->GetBuffer(), base_bytes, shared_memory_->GetHostBuffer(),
      base_bytes, size_bytes);
  return true;
}

bool D3D12CommandProcessor::CreateReadbackStagingBuffer(
    ReadbackStagingBuffer& buffer, uint32_t size) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource* resource;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesReadback,
          provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)))) {
    XELOGE("Failed to create a {} KB readback staging buffer", size >> 10);
    return false;
  }
  // Persistently mapped, and the whole thing may be read.
  if (FAILED(resource->Map(0, nullptr, &buffer.mapped))) {
    XELOGE("Failed to map a {} KB readback staging buffer", size >> 10);
    buffer.mapped = nullptr;
    resource->Release();
    return false;
  }
  resource->SetName(L"Readback Staging Buffer");
  buffer.resource.Attach(resource);
  return true;
}

void D3D12CommandProcessor::DestroyReadbackStagingBuffer(
    ReadbackStagingBuffer& buffer) {
  if (!buffer.resource) {
    return;
  }
  if (buffer.mapped != nullptr) {
    buffer.resource->Unmap(0, nullptr);
    buffer.mapped = nullptr;
  }
  // Deferred, a submitted copy may still be writing it.
  resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                       buffer.resource.Detach());
}

bool D3D12CommandProcessor::FaultCopyResolveToGuestRam(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (ranges.empty()) {
    return false;
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  if (!resolve_fault_command_list_) {
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&resolve_fault_command_allocator_))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            resolve_fault_command_allocator_.Get(), nullptr,
            IID_PPV_ARGS(&resolve_fault_command_list_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&resolve_fault_fence_)))) {
      XELOGE(
          "D3D12CommandProcessor: Failed to create the fault copy command "
          "list");
      DestroyResolveFaultCopy();
      return false;
    }
    // Created open.
    resolve_fault_command_list_->Close();
  }
  // The queue is free-threaded, so this orders after whatever the GPU thread
  // has executed on it, including the resolve.
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();
  // With zero-copy the resolve wrote guest RAM itself, so there is nothing to
  // copy, only the GPU to wait for.
  bool staging = false;
  if (!shared_memory_->is_zero_copy()) {
    if (FAILED(resolve_fault_command_allocator_->Reset()) ||
        FAILED(resolve_fault_command_list_->Reset(
            resolve_fault_command_allocator_.Get(), nullptr))) {
      return false;
    }
    ID3D12Resource* source = shared_memory_->GetBuffer();
    ID3D12Resource* dest = shared_memory_->GetHostBuffer();
    // Without the host buffer, the ranges are packed into a readback buffer and
    // copied out to guest RAM here once the GPU is done.
    staging = dest == nullptr;
    if (staging) {
      uint32_t staging_length = 0;
      for (const auto& range : ranges) {
        staging_length += range.second;
      }
      if (!EnsureResolveFaultReadback(staging_length)) {
        return false;
      }
      dest = resolve_fault_readback_.resource.Get();
    }
    // Buffers decay to COMMON when an ExecuteCommandLists completes, the GPU
    // thread's included, so that is their state here, whatever the GPU thread
    // tracks. Explicit transitions rather than implicit promotion, and back to
    // COMMON after, for drivers to rely on as little as possible. A readback
    // buffer stays in COPY_DEST.
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = dest;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    UINT barrier_count = staging ? 1 : 2;
    resolve_fault_command_list_->ResourceBarrier(barrier_count, barriers);
    uint32_t staging_offset = 0;
    for (const auto& range : ranges) {
      resolve_fault_command_list_->CopyBufferRegion(
          dest, staging ? staging_offset : range.first, source, range.first,
          range.second);
      staging_offset += range.second;
    }
    for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    resolve_fault_command_list_->ResourceBarrier(barrier_count, barriers);
    if (FAILED(resolve_fault_command_list_->Close())) {
      return false;
    }
    ID3D12CommandList* execute_command_lists[] = {
        resolve_fault_command_list_.Get()};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
  }
  ++resolve_fault_fence_value_;
  if (FAILED(direct_queue->Signal(resolve_fault_fence_.Get(),
                                  resolve_fault_fence_value_)) ||
      FAILED(resolve_fault_fence_->SetEventOnCompletion(
          resolve_fault_fence_value_, nullptr))) {
    return false;
  }
  if (staging) {
    CopyPackedRangesToGuestRam(resolve_fault_readback_.mapped, ranges);
  }
  return true;
}

bool D3D12CommandProcessor::EnsureResolveFaultReadback(uint32_t size) {
  if (resolve_fault_readback_.resource &&
      resolve_fault_readback_size_ >= size) {
    return true;
  }
  ReleaseResolveFaultReadback();
  uint32_t buffer_size = AlignReadbackBufferSize(size);
  if (!CreateReadbackStagingBuffer(resolve_fault_readback_, buffer_size)) {
    return false;
  }
  resolve_fault_readback_size_ = buffer_size;
  return true;
}

// Directly rather than through resources_for_deletion_, which is the GPU
// thread's, as no copy into it is in flight.
void D3D12CommandProcessor::ReleaseResolveFaultReadback() {
  if (resolve_fault_readback_.resource && resolve_fault_readback_.mapped) {
    resolve_fault_readback_.resource->Unmap(0, nullptr);
  }
  resolve_fault_readback_.mapped = nullptr;
  resolve_fault_readback_.resource.Reset();
  resolve_fault_readback_size_ = 0;
}

void D3D12CommandProcessor::DestroyResolveFaultCopy() {
  // Every copy is awaited before returning, so nothing is in flight.
  ReleaseResolveFaultReadback();
  resolve_fault_command_list_.Reset();
  resolve_fault_command_allocator_.Reset();
  resolve_fault_fence_.Reset();
  resolve_fault_fence_value_ = 0;
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type,
                                      uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw.
    // TODO(Triang3l): Do something so memexport still works in this case maybe?
    // Unlikely that zero would even really be legal though.
    return true;
  }

  // Vertex shader analysis.
  auto vertex_shader = static_cast<SpirvShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);

  const bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  SpirvShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<SpirvShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }

  const bool memexport_used_pixel =
      pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  const bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    return false;
  }

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return true;
  }

  // Two-buffer memexport routing: producer draws (memexport_used) and geometry
  // draws consuming memexport output use the host buffer (aliasing guest RAM)
  // so the output stays CPU coherent and consumers read it directly. Only
  // texture-sampled ranges are copied into the device buffer on demand. Off
  // without the host buffer, where the staging readback carries the output
  // instead, and when memexport_enable asks for device-local output.
  bool route_to_host = false;
  if (cvars::memexport_enable && shared_memory_->GetHostBuffer() != nullptr) {
    route_to_host =
        memexport_used ||
        (any_memexport_pages_written_ &&
         ((primitive_processing_result.index_buffer_type ==
               PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
           IsMemexportRange(
               primitive_processing_result.guest_index_base,
               primitive_processing_result.guest_draw_vertex_count *
                   uint32_t(sizeof(uint32_t)))) ||
          VertexFetchInMemexportRange(regs, *vertex_shader)));
  }

  reg::RB_DEPTHCONTROL normalized_depth_control =
      draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(
                         regs, pixel_shader->writes_color_targets())
                   : 0;
  draw_util::HostDepthPolygonOffset host_depth_polygon_offset;
  bool apply_host_depth_polygon_offset =
      pixel_shader && !pixel_shader->writes_depth() &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kHostRenderTargets &&
      draw_util::GetHostDepthPolygonOffsetIfNeeded(
          regs, primitive_polygonal, normalized_depth_control,
          normalized_color_mask, host_depth_polygon_offset);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(
                          regs.Get<reg::SQ_PROGRAM_CNTL>(),
                          regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
                   : 0;
  // The canonical D3D12 pipeline modification is the SPIR-V (spirv_to_dxil)
  // modification - it is what the guest shaders are actually translated with.
  SpirvShaderTranslator::Modification vertex_shader_modification(
      pipeline_cache_->GetCurrentSpirvVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type,
          interpolator_mask));
  SpirvShaderTranslator::Modification pixel_shader_modification(
      pixel_shader ? pipeline_cache_->GetCurrentSpirvPixelShaderModification(
                         *pixel_shader, interpolator_mask, ps_param_gen_pos,
                         normalized_depth_control, normalized_color_mask,
                         apply_host_depth_polygon_offset)
                   : uint64_t(0));
  // Hybrid occlusion query draw, counting coverage into the Total counter.
  // Only depth or stencil tested draws w/o depth writes, so scene geometry
  // keeps early depth rejection, and nothing can fail without a test anyway.
  bool zpd_hybrid = zpd_hybrid_supported_ &&
                    (zpd_active_segment_.segment_active ||
                     zpd_active_segment_.segment_pending_begin) &&
                    !normalized_depth_control.z_write_enable &&
                    (normalized_depth_control.z_enable ||
                     normalized_depth_control.stencil_enable);
  // For drawing w/o the counting while the pipeline is still being created.
  SpirvShaderTranslator::Modification pixel_shader_modification_without_zpd =
      pixel_shader_modification;
  if (zpd_hybrid && pixel_shader) {
    pixel_shader_modification.pixel.set_zpd_total(true);
    // The counter UAV write disables early depth/stencil.
    if (pixel_shader_modification.pixel.depth_stencil_mode ==
        SpirvShaderTranslator::Modification::DepthStencilMode::kEarlyHint) {
      pixel_shader_modification.pixel.depth_stencil_mode =
          SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
    }
  }

  // Set up the render targets - this may perform dispatches and draws.
  if (!render_target_cache_->Update(is_rasterization_done,
                                    normalized_depth_control,
                                    normalized_color_mask, *vertex_shader)) {
    return false;
  }

  // Create the pipeline (for this, need the actually used render target formats
  // from the render target cache), translating the shaders - doing this now to
  // obtain the used textures.
  Shader::Translation* vertex_shader_translation =
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value);
  Shader::Translation* pixel_shader_translation =
      pixel_shader ? pixel_shader->GetOrCreateTranslation(
                         pixel_shader_modification.value)
                   : nullptr;
  uint32_t bound_depth_and_color_render_target_bits;
  uint32_t bound_depth_and_color_render_target_formats
      [1 + xenos::kMaxColorRenderTargets];
  bool host_render_targets_used = render_target_cache_->GetPath() ==
                                  RenderTargetCache::Path::kHostRenderTargets;
  if (host_render_targets_used) {
    bound_depth_and_color_render_target_bits =
        render_target_cache_->GetLastUpdateBoundRenderTargets(
            bound_depth_and_color_render_target_formats);
  } else {
    bound_depth_and_color_render_target_bits = 0;
  }
  // The ucode interpreter can rasterize a not-yet-translated vertex shader
  // while its real shaders compile in the background (mirrors the Vulkan path).
  // Eligible only for plain non-expanded vertex shaders with no textures /
  // subroutines / memexport / loops, which the interpreter core handles.
  bool use_interpreter =
      cvars::async_shader_vs_interpreter &&
      !vertex_shader_translation->is_translated() &&
      active_vertex_shader_ucode_address() != 0 &&
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kVertex &&
      primitive_processing_result.host_primitive_type !=
          xenos::PrimitiveType::kPointList &&
      primitive_processing_result.host_primitive_type !=
          xenos::PrimitiveType::kRectangleList &&
      primitive_processing_result.host_primitive_type !=
          xenos::PrimitiveType::kQuadList &&
      vertex_shader->texture_bindings().empty() &&
      !vertex_shader->uses_subroutine_calls() &&
      vertex_shader->memexport_eM_written() == 0 &&
      vertex_shader->constant_register_map().loop_bitmap == 0;
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation,
          primitive_processing_result, normalized_depth_control,
          normalized_color_mask, apply_host_depth_polygon_offset, zpd_hybrid,
          bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, use_interpreter,
          &pipeline_handle, &root_signature)) {
    return false;
  }

  if (cvars::async_shader_compilation) {
    if (zpd_hybrid &&
        pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
      // The counting pipeline isn't ready - draw without counting rather than
      // stall, and re-configure for the plain modification.
      zpd_hybrid = false;
      pixel_shader_modification = pixel_shader_modification_without_zpd;
      if (pixel_shader) {
        pixel_shader_translation = pixel_shader->GetOrCreateTranslation(
            pixel_shader_modification.value);
      }
      if (!pipeline_cache_->ConfigurePipeline(
              vertex_shader_translation, pixel_shader_translation,
              primitive_processing_result, normalized_depth_control,
              normalized_color_mask, apply_host_depth_polygon_offset, false,
              bound_depth_and_color_render_target_bits,
              bound_depth_and_color_render_target_formats, use_interpreter,
              &pipeline_handle, &root_signature)) {
        return false;
      }
    }
    if (zpd_active_segment_.segment_active ||
        zpd_active_segment_.segment_pending_begin) {
      // Occlusion-query draws need the real pixel shader - the no-op
      // placeholder skips the guest shader's pixel kills and would miscount.
      // Wait for it.
      if (pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) ==
              nullptr ||
          pipeline_cache_->IsPlaceholderPipeline(pipeline_handle)) {
        if (cvars::occlusion_query_log) {
          XELOGI(
              "ZPD: Awaiting real D3D12 pipeline for active query draw "
              "VS={:016X} PS={:016X}",
              vertex_shader ? vertex_shader->ucode_data_hash() : 0,
              pixel_shader ? pixel_shader->ucode_data_hash() : 0);
        }
        if (pipeline_cache_->AwaitRealD3D12PipelineByHandle(pipeline_handle) ==
            nullptr) {
          XELOGE(
              "IssueDraw: Pipeline unavailable after await for active query "
              "draw VS={:016X} PS={:016X}",
              vertex_shader ? vertex_shader->ucode_data_hash() : 0,
              pixel_shader ? pixel_shader->ucode_data_hash() : 0);
          return false;
        }
      }
    } else if (pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) ==
               nullptr) {
      // No pipeline and no placeholder available (async_shader_skip_draws with
      // no interpreter stand-in, bindful async, or a failed placeholder) - skip
      // the draw until the real pipeline is ready.
      XELOGI(
          "Skipping draw - pipeline not ready: VS {:016X} mod {:016X}, PS "
          "{:016X} mod {:016X}",
          vertex_shader->ucode_data_hash(), vertex_shader_modification.value,
          pixel_shader ? pixel_shader->ucode_data_hash() : 0,
          pixel_shader_modification.value);
      return true;
    }
  }
  // The interpreter reads the guest ucode from shared memory by its program
  // address. A cached interpreter placeholder reused for an inline
  // (IM_LOAD_IMMEDIATE, address 0) shader can't be fed, so skip until the real
  // pipeline is ready rather than interpret from address 0.
  if (active_vertex_shader_ucode_address() == 0) {
    bool is_placeholder = false;
    bool is_interpreter_placeholder = false;
    pipeline_cache_->GetD3D12PipelineForDraw(pipeline_handle, &is_placeholder,
                                             &is_interpreter_placeholder);
    if (is_interpreter_placeholder) {
      return true;
    }
  }

  // Push debug marker with Xbox 360 draw context for PIX/RenderDoc annotation.
  // Done early so texture loads appear nested under the draw that uses them.
  if (debug_markers_enabled_ || cvars::log_draws) {
    char label[draw_util::kDebugMarkerLabelMaxLength];
    draw_util::FormatDrawDebugMarker(
        label, sizeof(label), primitive_type, primitive_processing_result,
        vertex_shader ? vertex_shader->ucode_data_hash() : 0,
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    PushDebugMarker(memexport_used ? "%s (memexport)" : "%s", label);
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr
           ? pixel_shader->GetUsedTextureMaskAfterTranslation()
           : 0);
  texture_cache_->RequestTextures(used_texture_mask);

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines. For a placeholder, pin the concrete PSO instead of the
  // swappable handle: the handle is resolved again when the deferred command
  // list is replayed, so a real pipeline hot-swapped in before the submission
  // would run against the bindings uploaded below for the placeholder - an
  // empty bindless index buffer for the still-translating pixel shader, and
  // (interpreter) full-256 float constants in place of the packed layout the
  // real vertex shader reads.
  bool placeholder_pipeline = false;
  bool interpreter_placeholder = false;
  ID3D12PipelineState* draw_pipeline_state =
      pipeline_cache_->GetD3D12PipelineForDraw(
          pipeline_handle, &placeholder_pipeline, &interpreter_placeholder);
  if (placeholder_pipeline) {
    if (current_external_pipeline_ != draw_pipeline_state) {
      deferred_command_list_.D3DSetPipelineState(draw_pipeline_state);
      current_external_pipeline_ = draw_pipeline_state;
      current_guest_pipeline_ = nullptr;
    }
  } else if (current_guest_pipeline_ != pipeline_handle) {
    deferred_command_list_.SetPipelineStateHandle(
        reinterpret_cast<void*>(pipeline_handle));
    current_guest_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state. Using the resolution scale of this draw,
  // which may be 1x1 because of draw_resolution_scale_threshold.
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();
  // ZPD segments can't mix scales or hybrid query Total counting.
  // The resolved sample count is divided by one scale area per segment.
  // Needs to be split before the counter index goes into system constants.
  UpdateZPDSegment(draw_resolution_scale_x * draw_resolution_scale_y,
                   zpd_hybrid);
  draw_util::ViewportInfo viewport_info;
  draw_util::GetViewportInfoArgs gviargs{};

  gviargs.Setup(
      draw_resolution_scale_x, draw_resolution_scale_y,
      draw_resolution_scale_x > 1
          ? texture_cache_->draw_resolution_scale_x_divisor()
          : divisors::MagicDiv(1),
      draw_resolution_scale_y > 1
          ? texture_cache_->draw_resolution_scale_y_divisor()
          : divisors::MagicDiv(1),
      true, D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
      normalized_depth_control,
      host_render_targets_used &&
          render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);

  if (gviargs == previous_viewport_info_args_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(&gviargs, viewport_info);
    previous_viewport_info_args_ = gviargs;
    previous_viewport_info_ = viewport_info;
  }
  // todo: use SIMD for getscissor + scaling here, should reduce code size more
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
#if XE_ARCH_AMD64 == 1
  __m128i* scisp = (__m128i*)&scissor;
  *scisp = _mm_mullo_epi32(
      *scisp, _mm_setr_epi32(draw_resolution_scale_x, draw_resolution_scale_y,
                             draw_resolution_scale_x, draw_resolution_scale_y));
#else
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
#endif
  // Update viewport, scissor, blend factor and stencil reference.
  UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal,
                           normalized_depth_control, normalized_color_mask,
                           bound_depth_and_color_render_target_bits);

  // The spirv_to_dxil guest path fills SPIR-V system constants and binds the
  // Mesa root signature itself.
  if (!UpdateBindingsMesa(
          vertex_shader, pixel_shader, memexport_used, route_to_host,
          primitive_polygonal, primitive_processing_result, viewport_info,
          normalized_depth_control, normalized_color_mask,
          apply_host_depth_polygon_offset ? &host_depth_polygon_offset
                                          : nullptr,
          interpreter_placeholder)) {
    return false;
  }
  // Must not call anything that can change the descriptor heap from now on!

  // Ensure vertex buffers are resident.
  // TODO(Triang3l): Cache residency for ranges in a way similar to how texture
  // validity is tracked.
  const Shader::ConstantRegisterMap& constant_map_vertex =
      vertex_shader->constant_register_map();
  {
    uint32_t vfetch_addresses[96];
    uint32_t vfetch_sizes[96];
    uint32_t vfetch_current_queued = 0;
    for (uint32_t i = 0;
         i < xe::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining = xe::clear_lowest_bit(vfetch_bits_remaining);
        uint32_t vfetch_index = i * 32 + j;
        xenos::xe_gpu_vertex_fetch_t vfetch_constant =
            regs.GetVertexFetch(vfetch_index);
        switch (vfetch_constant.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (cvars::gpu_allow_invalid_fetch_constants) {
              break;
            }
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
                "type! "
                "This is incorrect behavior, but you can try bypassing this by "
                "launching Xenia with "
                "--gpu_allow_invalid_fetch_constants=true.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return false;
          default:
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) is completely "
                "invalid!",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return false;
        }
        // Mask to physical like the shader - the guest may use a mirror window.
        vfetch_addresses[vfetch_current_queued] =
            xenos::CpuToGpu(vfetch_constant.address << 2) >> 2;
        vfetch_sizes[vfetch_current_queued++] = vfetch_constant.size;
      }
    }

    if (vfetch_current_queued) {
      // so far, i have never seen vfetch_current_queued > 4. 1 is most common,
      // 2 happens occasionally. did not test many games though pre-acquire the
      // critical region so we're not repeatedly re-acquiring it in requestrange
      auto shared_memory_request_range_hoisted =
          global_critical_region::Acquire();

      for (uint32_t i = 0; i < vfetch_current_queued; ++i) {
        if (!shared_memory_->RequestRange(vfetch_addresses[i] << 2,
                                          vfetch_sizes[i] << 2)) {
          XELOGE(
              "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
              "shared memory",
              vfetch_addresses[i] << 2, vfetch_sizes[i] << 2);
          return false;
        }
      }
    }
  }
  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  // Host-routed producers write output to host_buffer_ (guest RAM), not the
  // device buffer, so this upload is redundant. It also drops the draw when the
  // guest committed only part of the declared capacity, so skip it.
  if (!route_to_host) {
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      if (!shared_memory_->RequestRange(
              memexport_range.base_address_dwords << 2,
              memexport_range.size_bytes)) {
        XELOGE(
            "Failed to request memexport stream at 0x{:08X} (size {}) in the "
            "shared memory",
            memexport_range.base_address_dwords << 2,
            memexport_range.size_bytes);
        return false;
      }
    }
  }
  // Primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      // TODO(Triang3l): Support all primitive types.
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode ==
             xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode ==
             xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        XELOGE(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        XELOGE(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  }
  SetPrimitiveTopology(primitive_topology);
  // Must not call anything that may change the primitive topology from now on!

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    if (!memexport_used) {
      route_to_host ? shared_memory_->UseHostForReading()
                    : shared_memory_->UseForReading();
    } else {
      route_to_host ? shared_memory_->UseHostForWriting()
                    : shared_memory_->UseForWriting();
    }
    SubmitBarriers();
    deferred_command_list_.D3DDrawInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.SizeInBytes =
        primitive_processing_result.host_draw_vertex_count;
    if (primitive_processing_result.host_index_format ==
        xenos::IndexFormat::kInt16) {
      index_buffer_view.SizeInBytes *= sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
    } else {
      index_buffer_view.SizeInBytes *= sizeof(uint32_t);
      index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
        if (memexport_used) {
          // If the shared memory is a UAV, it can't be used as an index buffer
          // (UAV is a read/write state, index buffer is a read-only state).
          // Need to copy the indices to a buffer in the index buffer state.
          scratch_index_buffer = RequestScratchGPUBuffer(
              index_buffer_view.SizeInBytes, D3D12_RESOURCE_STATE_COPY_DEST);
          if (scratch_index_buffer == nullptr) {
            return false;
          }
          route_to_host ? shared_memory_->UseHostAsCopySource()
                        : shared_memory_->UseAsCopySource();
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              scratch_index_buffer, 0,
              route_to_host ? shared_memory_->GetHostBuffer()
                            : shared_memory_->GetBuffer(),
              primitive_processing_result.guest_index_base,
              index_buffer_view.SizeInBytes);
          PushTransitionBarrier(scratch_index_buffer,
                                D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER);
          index_buffer_view.BufferLocation =
              scratch_index_buffer->GetGPUVirtualAddress();
        } else {
          index_buffer_view.BufferLocation =
              (route_to_host ? shared_memory_->GetHostGPUAddress()
                             : shared_memory_->GetGPUAddress()) +
              primitive_processing_result.guest_index_base;
        }
      } break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer_view.BufferLocation =
            primitive_processor_->GetConvertedIndexBufferGpuAddress(
                primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer_view.BufferLocation =
            primitive_processor_->GetBuiltinIndexBufferGpuAddress(
                primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
    if (memexport_used) {
      route_to_host ? shared_memory_->UseHostForWriting()
                    : shared_memory_->UseForWriting();
    } else {
      route_to_host ? shared_memory_->UseHostForReading()
                    : shared_memory_->UseForReading();
    }
    SubmitBarriers();
    deferred_command_list_.D3DDrawIndexedInstanced(
        primitive_processing_result.host_draw_vertex_count, 1, 0, 0, 0);
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer,
                              D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }

  // Pop debug marker for draw call.
  PopDebugMarker();

  if (memexport_used) {
    InsertDebugMarker("Memexport draw: %zu ranges", memexport_ranges_.size());
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    route_to_host ? shared_memory_->MarkHostUAVWritesCommitNeeded()
                  : shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      shared_memory_->RangeWrittenByGpu(
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes,
          !route_to_host);
    }
    if (route_to_host) {
      // Output landed in host_buffer_ (guest RAM), already CPU coherent, so no
      // readback. Record the written pages so geometry consumers route to the
      // host buffer and texture loads copy their range into the device buffer.
      for (const draw_util::MemExportRange& memexport_range :
           memexport_ranges_) {
        MarkMemexportPagesWritten(memexport_range.base_address_dwords << 2,
                                  memexport_range.size_bytes);
      }
    } else if (cvars::memexport_enable && !shared_memory_->is_zero_copy()) {
      // No host buffer to route to, and buffer_ is device-local, so the CPU
      // only sees the output if it is read back. Under zero-copy buffer_ is
      // guest RAM already, and a readback there would clobber it.
      StageMemexportReadback();
    }
  }

  return true;
}

// Records copies of this draw's export output into staging buffers. The copy
// out waits, so it is deferred to FlushMemexportStagingReadback, where one wait
// covers every producer since the last one.
void D3D12CommandProcessor::StageMemexportReadback() {
  if (memexport_ranges_.empty()) {
    return;
  }
  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* device_buffer = shared_memory_->GetBuffer();
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t base_bytes = memexport_range.base_address_dwords << 2;
    if (base_bytes >= SharedMemory::kBufferSize) {
      continue;
    }
    uint32_t size_bytes = std::min(memexport_range.size_bytes,
                                   SharedMemory::kBufferSize - base_bytes);
    // The declared capacity can run past what the guest committed, and the
    // copy out would write guest memory that isn't there.
    size_bytes = WritableGuestRangeLength(base_bytes, size_bytes);
    if (!size_bytes) {
      continue;
    }
    uint64_t key = MakeReadbackResolveKey(base_bytes, size_bytes);
    ReadbackStagingSlot* slot = AcquireReadbackStagingSlot(key, size_bytes);
    if (slot == nullptr) {
      continue;
    }
    InsertDebugMarker("Memexport Readback (staging): 0x%08X, %u bytes",
                      base_bytes, size_bytes);
    deferred_command_list_.D3DCopyBufferRegion(
        slot->buffer.resource.Get(), 0, device_buffer, base_bytes, size_bytes);
    // Its buffer now holds the newer output, so keep one entry, at the back -
    // copy out order is what decides overlapping ranges.
    std::erase_if(memexport_staged_, [key](const MemexportStagedRange& staged) {
      return staged.key == key;
    });
    memexport_staged_.push_back({key, base_bytes, size_bytes});
    // The fence and coherency waits are driven by the page marks.
    MarkMemexportPagesWritten(base_bytes, size_bytes);
  }
}

void D3D12CommandProcessor::FlushMemexportStagingReadback() {
  if (memexport_staged_.empty()) {
    return;
  }
  // Staged output is about to reach guest RAM, so no fence need await it.
  memexport_await_pending_ = false;
  if (!AwaitAllQueueOperationsCompletion()) {
    XELOGE(
        "D3D12CommandProcessor: Failed to complete queue operations for "
        "memexport staging readback");
    memexport_staged_.clear();
    return;
  }
  for (const MemexportStagedRange& staged : memexport_staged_) {
    ReadbackStagingSlot* slot = FindReadbackStagingSlot(staged.key);
    if (slot == nullptr) {
      continue;
    }
    // The guest may have decommitted the range since the draw that staged it.
    uint32_t length = WritableGuestRangeLength(staged.address, staged.length);
    if (!length) {
      continue;
    }
    ReadbackStagingToGuestRam(slot->buffer, staged.address, length);
  }
  memexport_staged_.clear();
}

void D3D12CommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(false)) {
    return;
  }
  bool render_target_cache_submitted =
      render_target_cache_->InitializeTraceSubmitDownloads();
  bool shared_memory_submitted =
      shared_memory_->InitializeTraceSubmitDownloads();
  if (!render_target_cache_submitted && !shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (render_target_cache_submitted) {
    render_target_cache_->InitializeTraceCompleteDownloads();
  }
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

bool D3D12CommandProcessor::DumpEdramSnapshotToFile(
    const std::filesystem::path& path) {
  if (!BeginSubmission(false)) {
    return false;
  }
  if (!render_target_cache_->InitializeTraceSubmitDownloads()) {
    return false;
  }
  AwaitAllQueueOperationsCompletion();
  return render_target_cache_->WriteEdramSnapshotToFile(path);
}

void D3D12CommandProcessor::ResolveReadCallbackThunk(
    void* context, uint32_t physical_address, uint32_t length,
    Memory::PhysicalAccess access) {
  auto command_processor = static_cast<D3D12CommandProcessor*>(context);
  switch (access) {
    case Memory::PhysicalAccess::kRead:
      command_processor->MarkResolvePagesRead(physical_address, length);
      break;
    case Memory::PhysicalAccess::kWrite:
      command_processor->PrepareResolvePagesForWrite(physical_address, length);
      break;
    case Memory::PhysicalAccess::kDiscard:
      command_processor->DiscardResolvePages(physical_address, length);
      break;
  }
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  if (!BeginSubmission(true)) {
    return false;
  }

  // Push debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PushDebugMarker("IssueCopy (Resolve)");
  }

  bool result;
  if (!IsReadbackResolveEnabled()) {
    uint32_t written_address, written_length;
    result = render_target_cache_->Resolve(*memory_, *shared_memory_,
                                           *texture_cache_, written_address,
                                           written_length);
    if (result) {
      // The resolve wrote the device buffer. Drop any stale memexport marks so
      // the output isn't overwritten with guest RAM by a later texture load.
      ClearMemexportPages(written_address, written_length);
    }
  } else {
    result = IssueCopy_ReadbackResolvePath();
  }

  // Pop debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PopDebugMarker();
  }

  SubmitRequestedResolve();

  return result;
}

// Downscales a scaled resolve's output into resolve_downscale_buffer_, leaving
// it a copy source inside a debug marker scope, for the caller to copy out of
// it and restore.
bool D3D12CommandProcessor::DownscaleScaledResolve(
    uint32_t written_address, const ScaledResolveReadbackInfo& scaled_info) {
  uint32_t pixel_size_log2 = scaled_info.pixel_size_log2;
  uint32_t tile_count = scaled_info.tile_count;
  uint32_t readback_length = scaled_info.readback_length;
  uint32_t scale_x = scaled_info.scale_x;
  uint32_t scale_y = scaled_info.scale_y;
  uint64_t scaled_start = scaled_info.scaled_start;
  uint64_t scaled_readback_length = scaled_info.scaled_readback_length;

  // Ensure intermediate buffer for GPU downscaling is large enough
  uint32_t downscale_buffer_size = AlignReadbackBufferSize(readback_length);
  if (downscale_buffer_size > resolve_downscale_buffer_size_) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(
        buffer_desc, downscale_buffer_size,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* buffer;
    if (SUCCEEDED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesDefault,
            provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&buffer)))) {
      // Defer release of old buffer - it may still be referenced by pending
      // deferred commands from previous resolves
      if (resolve_downscale_buffer_) {
        resources_for_deletion_.emplace_back(
            GetCurrentSubmission(), resolve_downscale_buffer_.Detach());
      }
      resolve_downscale_buffer_.Attach(buffer);
      resolve_downscale_buffer_->SetName(L"Resolve Downscale Buffer");
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    } else {
      XELOGE("Failed to create {} MB resolve downscale buffer",
             downscale_buffer_size >> 20);
      return false;
    }
  }

  // Verify downscale buffer was created
  if (!resolve_downscale_buffer_) {
    XELOGE("Resolve downscale: downscale buffer is null");
    return false;
  }

  // Get source buffer
  size_t resolve_buffer_index =
      texture_cache_->GetCurrentScaledResolveBufferIndexPublic();
  ID3D12Resource* resolve_buffer =
      texture_cache_->GetCurrentScaledResolveBufferResource();
  if (!resolve_buffer) {
    XELOGE("Resolve downscale: source buffer is null");
    return false;
  }

  // Allocate descriptors for SRV (source) and UAV (destination)
  ui::d3d12::util::DescriptorCpuGpuHandlePair downscale_descriptors[2];
  if (!RequestOneUseSingleViewDescriptors(2, downscale_descriptors)) {
    XELOGE("Failed to allocate descriptors for resolve downscale");
    return false;
  }

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Create SRV for source (the written extent within the scaled resolve
  // buffer). The shader reads from the start of the bound range, so
  // source_offset_bytes stays 0.
  uint64_t source_offset =
      scaled_start - (uint64_t(resolve_buffer_index) << 30);
  uint32_t aligned_source_length = (uint32_t(scaled_readback_length) +
                                    (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1)) &
                                   ~(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1);
  ui::d3d12::util::CreateBufferRawSRV(device, downscale_descriptors[0].first,
                                      resolve_buffer, aligned_source_length,
                                      source_offset);

  // Create UAV for destination (downscale buffer)
  uint32_t aligned_readback_length =
      (readback_length + (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1)) &
      ~(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1);
  ui::d3d12::util::CreateBufferRawUAV(device, downscale_descriptors[1].first,
                                      resolve_downscale_buffer_.Get(),
                                      aligned_readback_length, 0);

  // Transition source to SRV state
  PushUAVBarrier(resolve_buffer);
  texture_cache_->TransitionCurrentScaledResolveRange(
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  SubmitBarriers();

  PushDebugMarker("Resolve Downscale: 0x%08X, %u bytes -> %u bytes",
                  written_address, uint32_t(scaled_readback_length),
                  readback_length);

  // Set up compute shader
  SetExternalPipeline(resolve_downscale_pipeline_.Get());
  deferred_command_list_.D3DSetComputeRootSignature(
      resolve_downscale_root_signature_.Get());

  // Set constants
  ResolveDownscaleConstants constants;
  constants.scale_x = scale_x;
  constants.scale_y = scale_y;
  constants.pixel_size_log2 = pixel_size_log2;
  constants.tile_count = tile_count;
  // The source SRV is already created at source_offset, so the shader reads
  // from the start of the bound range.
  constants.source_offset_bytes = 0;
  // Optionally sample from center of scaled block instead of top-left.
  constants.half_pixel_offset = (cvars::readback_resolve_half_pixel_offset &&
                                 (scale_x > 1 || scale_y > 1))
                                    ? 1
                                    : 0;
  deferred_command_list_.D3DSetComputeRoot32BitConstants(
      UINT(ResolveDownscaleRootParameter::kConstants),
      sizeof(constants) / sizeof(uint32_t), &constants, 0);

  // Set descriptor tables
  deferred_command_list_.D3DSetComputeRootDescriptorTable(
      UINT(ResolveDownscaleRootParameter::kSource),
      downscale_descriptors[0].second);
  deferred_command_list_.D3DSetComputeRootDescriptorTable(
      UINT(ResolveDownscaleRootParameter::kDestination),
      downscale_descriptors[1].second);

  // Dispatch compute shader - one thread group per 32x32 tile
  deferred_command_list_.D3DDispatch(tile_count, 1, 1);

  PushUAVBarrier(resolve_downscale_buffer_.Get());
  PushTransitionBarrier(resolve_downscale_buffer_.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
  return true;
}

// Writes a scaled resolve's output, downscaled, into the shared memory buffer,
// where a native resolve would have written it, so it can be read back like
// one. The resolve has already marked the range as written by the GPU.
bool D3D12CommandProcessor::MirrorScaledResolveToSharedMemory(
    uint32_t written_address, uint32_t written_length,
    reg::RB_COPY_DEST_INFO copy_dest_info, uint32_t& mirrored_length_out) {
  if (!resolve_downscale_pipeline_ || !resolve_downscale_root_signature_) {
    return false;
  }
  ScaledResolveReadbackInfo scaled_info;
  if (!GetScaledResolveReadbackInfo(written_address, written_length,
                                    copy_dest_info, scaled_info)) {
    return false;
  }
  // Only for the memory backing the range, the pages are already valid.
  if (!shared_memory_->RequestRange(written_address,
                                    scaled_info.readback_length)) {
    return false;
  }
  if (!DownscaleScaledResolve(written_address, scaled_info)) {
    return false;
  }
  shared_memory_->UseAsCopyDestination();
  SubmitBarriers();
  deferred_command_list_.D3DCopyBufferRegion(
      shared_memory_->GetBuffer(), written_address,
      resolve_downscale_buffer_.Get(), 0, scaled_info.readback_length);
  PushTransitionBarrier(resolve_downscale_buffer_.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  texture_cache_->TransitionCurrentScaledResolveRange(
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  SubmitBarriers();
  PopDebugMarker();
  texture_cache_->MarkScaledResolveMirrored(written_address,
                                            scaled_info.readback_length);
  mirrored_length_out = scaled_info.readback_length;
  return true;
}

XE_NOINLINE
bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  uint32_t written_address, written_length;
  reg::RB_COPY_DEST_INFO copy_dest_info;
  bool is_scaled;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                     written_address, written_length,
                                     &copy_dest_info, &is_scaled)) {
    return false;
  }

  if (!written_length) {
    return true;
  }
  // Readback keeps guest RAM coherent, so this is only for accuracy, but a
  // resolved page is no longer memexport output either way.
  ClearMemexportPages(written_address, written_length);

  if (!IsResolveDestinationResident(written_address, written_length)) {
    return true;
  }
  // A CPU access copies the output into guest RAM out of the shared memory
  // buffer, so scaled output has to be there too. With zero-copy that buffer is
  // guest RAM, where native resolves land in place as well.
  if (is_scaled) {
    uint32_t mirrored_length;
    if (MirrorScaledResolveToSharedMemory(written_address, written_length,
                                          copy_dest_info, mirrored_length)) {
      is_scaled = false;
      written_length = mirrored_length;
    }
  }
  NoteResolveForReadback(written_address, written_length, is_scaled);
  return true;
}

void D3D12CommandProcessor::CheckSubmissionCompletion(
    uint64_t await_submission) {
  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(queue_operations_since_submission_completion_timeline_
                        ->SignalAndAdvance(direct_queue)) &&
          queue_operations_since_submission_completion_timeline_
              ->AwaitAllSubmissions()) {
        queue_operations_done_since_submission_signal_ = false;
      } else {
        XELOGE(
            "Failed to await the completion of an out-of-submission "
            "Direct3D 12 queue operation");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  completion_timeline_->AwaitSubmissionAndUpdateCompleted(await_submission);

  const uint64_t completed_submission = GetCompletedSubmission();

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission >
        completed_submission) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next =
          command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ =
        command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second >
        completed_submission) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(
        view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > completed_submission) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(completed_submission);

  // Pull completed query resolves so ZPD reports can retire.
  PumpQueryResolves();
  PumpPendingRetire();
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    XELOGE("Direct3D 12 device removed, reason: 0x{:08X}",
           uint32_t(device_removed_reason));
    GetD3D12Provider().DumpDeviceRemovedData();
    if (cvars::d3d12_debug) {
      GetD3D12Provider().LogD3D12DebugMessages();
    }
    graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                 DXGI_ERROR_DEVICE_REMOVED);
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionCompletion(
      is_opening_frame
          ? closed_frame_submissions_[frame_current_ % kQueueFrames]
          : 0);
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ =
        std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] >
          GetCompletedSubmission()) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();

    // Resume the active query segment.
    OpenQuerySegment(false);

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Log guest ZPD report stats every 100 frames.
    if (GetZPDMode() != ZPDMode::kFake && cvars::occlusion_query_log &&
        zpd_host_query_pool_ && zpd_host_query_pool_->capacity() &&
        frame_current_ - zpd_stats_.last_log_frame >= 100) {
      XELOGI(
          "Occlusion Query Stats (last 100 frames): "
          "ReportsQueued={}, ReportsRetired={}, SegBegun={}, SegEnded={}, "
          "PoolExhausted={}, Failed={}, SpeculativeCorrections={}, "
          "RetiresAbandoned={}",
          zpd_stats_.reports_queued, zpd_stats_.reports_retired,
          zpd_stats_.segments_begun, zpd_stats_.segments_ended,
          zpd_stats_.pool_exhausted, zpd_stats_.failed,
          zpd_stats_.speculative_corrections, zpd_stats_.retires_abandoned);

      zpd_stats_.Reset(frame_current_);
    }

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0,
                sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    cbuffer_binding_runtime_data_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ =
          ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ =
          ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }

    pix_capturing_ =
        pix_capture_requested_.exchange(false, std::memory_order_relaxed);
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis =
          GetD3D12Provider().GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->BeginCapture();
      }
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&command_allocator)))) {
      XELOGE("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    // We can't close the command list with an active query - D3D12 requirement.
    // Close the active segment and emit ResolveQueryData before executing.
    if (zpd_mode_ != ZPDMode::kFake) {
      CloseQuerySegment();
      RecordZPDResolveBatch();
    }

    pipeline_cache_->EndSubmission();

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

    // TODO(Triang3l): Error checking.

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    command_allocator->Reset();
    command_list_->Reset(command_allocator, nullptr);
    deferred_command_list_.Execute(command_list_, command_list_1_,
                                   command_list_2_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    command_allocator_writable_first_->last_usage_submission =
        GetCurrentSubmission();
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next =
          command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }
    completion_timeline_->SignalAndAdvance(direct_queue);
    resolve_submitted_through_.store(GetCurrentSubmission() - 1,
                                     std::memory_order_release);
    OnResolveSubmissionEnded();

    submission_open_ = false;

    // Pump ZPD query process. This drains any resolves that became readable
    // from completed work and retires reports unblocked by those resolves.
    PumpQueryResolves();
    PumpPendingRetire();

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;

    // Flush debug-layer messages per submission.
    if (cvars::d3d12_debug) {
      provider.LogD3D12DebugMessages();
    }
  }

  if (is_closing_frame) {
    if (cvars::clear_memory_page_state) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    // Close the capture after submitting.
    if (pix_capturing_) {
      IDXGraphicsAnalysis* graphics_analysis = provider.GetGraphicsAnalysis();
      if (graphics_analysis != nullptr) {
        graphics_analysis->EndCapture();
      }
      pix_capturing_ = false;
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] =
        GetCurrentSubmission() - 1;
    // Backstop for export output no fence or coherency request has asked for,
    // so it can't sit staged indefinitely.
    FlushMemexportStagingReadback();
    EvictOldReadbackStaging();

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed :
             sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info,
    const draw_util::Scissor& scissor, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    if (!GetD3D12Provider().IsAlphaBlendFactorSupported()) {
      bool color_uses_constant_color = false;
      bool color_uses_constant_alpha = false;
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        // Ignore unbound targets and targets that don't write RGB, since their
        // color blend factors don't affect the host output merger.
        if (!(bound_depth_and_color_render_target_bits &
              (uint32_t(1) << (1 + i))) ||
            !((normalized_color_mask >> (i * 4)) & 0b0111)) {
          continue;
        }
        auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
            reg::RB_BLENDCONTROL::rt_register_indices[i]);
        // Direct3D 12 ignores blend factors for MIN and MAX.
        if (blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
            blend_control.color_comb_fcn == xenos::BlendOp::kMax) {
          continue;
        }
        const xenos::BlendFactor color_blend_factors[] = {
            blend_control.color_srcblend, blend_control.color_destblend};
        for (xenos::BlendFactor color_blend_factor : color_blend_factors) {
          switch (color_blend_factor) {
            case xenos::BlendFactor::kConstantColor:
            case xenos::BlendFactor::kOneMinusConstantColor:
              color_uses_constant_color = true;
              break;
            case xenos::BlendFactor::kConstantAlpha:
            case xenos::BlendFactor::kOneMinusConstantAlpha:
              color_uses_constant_alpha = true;
              break;
            default:
              break;
          }
        }
      }
      // Legacy D3D12 has only a four-component constant-color factor. If the
      // draw needs only the scalar constant-alpha factor, emulate it by
      // replicating A. Mixed constant-color and constant-alpha use can't be
      // represented exactly, so preserve the color factor in that case.
      if (color_uses_constant_alpha && !color_uses_constant_color) {
        blend_factor[0] = blend_factor[3];
        blend_factor[1] = blend_factor[3];
        blend_factor[2] = blend_factor[3];
      }
    }
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref =
        regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}
bool D3D12CommandProcessor::UpdateBindingsMesa(
    const SpirvShader* vertex_shader, const SpirvShader* pixel_shader,
    bool memexport_used, bool route_to_host, bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    const draw_util::ViewportInfo& viewport_info,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset,
    bool interpreter_placeholder) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  const RegisterFile& regs = *register_file_;

  ID3D12RootSignature* root_signature = GetMesaRootSignature();
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Resolution scale of this draw.
  // 1x1 with draw_resolution_scale_threshold (RTV only)
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  // Fill the SPIR-V system constants, mirroring
  // VulkanCommandProcessor::UpdateSystemConstantValues (the shared
  // SpirvShaderTranslator defines the layout). Serves both the host render
  // target and FSI paths. FSI EDRAM fields are filled below only when
  // edram_fsi.
  SpirvShaderTranslator::SystemConstants sc;
  std::memset(&sc, 0, sizeof(sc));
  sc.zpd_fsi_counter_index = UINT32_MAX;

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Flags. Vertex index loading is never needed. D3D12 supports full 32-bit
  // indices, so the guest shader uses SV_VertexID from the bound index buffer
  // directly (same in-shader endian swap and base index model as the Vulkan
  // backend).
  bool edram_fsi = render_target_cache_->GetPath() ==
                   RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // In the FSI path the linear->gamma encode happens on the EDRAM store, so the
  // pixel shader must not pre-encode (matches the Vulkan backend).
  if (!edram_fsi && !render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[i]);
      if (color_info.color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  sc.flags = flags;

  // Vertex index. The shader endian-swaps SV_VertexID and adds the base index.
  sc.vertex_index_endian = primitive_processing_result.host_shader_index_endian;
  sc.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  sc.vertex_index_count = primitive_processing_result.host_draw_vertex_count;

  // Interpreter placeholder: tell the interpreter VS where the guest ucode is
  // (dword base + CF instruction count) and make that shared memory resident.
  if (interpreter_placeholder) {
    uint32_t ucode_address = active_vertex_shader_ucode_address();
    sc.interpreter_ucode_base_dwords = ucode_address >> 2;
    sc.interpreter_cf_instr_count = vertex_shader->cf_pair_index_bound() * 2;
    shared_memory_->RequestRange(
        ucode_address,
        uint32_t(vertex_shader->ucode_dword_count()) * sizeof(uint32_t));
  }

  // Host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport_info.ndc_scale[i];
    sc.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // User clip planes.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
    float* write_ptr = sc.user_clip_planes[0];
    uint32_t planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t plane_index;
    while (xe::bit_scan_forward(planes_remaining, &plane_index)) {
      planes_remaining = xe::clear_lowest_bit(planes_remaining);
      if (plane_index >= 6) {
        continue;
      }
      std::memcpy(write_ptr, &regs[XE_GPU_REG_PA_CL_UCP_0_X + plane_index * 4],
                  4 * sizeof(float));
      write_ptr += 4;
    }
  }

  // Tessellation constants, read by the domain shader on tessellated draws.
  sc.tessellation_factor_range[0] =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sc.tessellation_factor_range[1] =
      regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  sc.tessellation_vertex_index_endian =
      uint32_t(primitive_processing_result.host_shader_index_endian);
  sc.tessellation_vertex_index_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
  sc.tessellation_vertex_index_min_max[0] = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
  sc.tessellation_vertex_index_min_max[1] = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];

  // Point size, and the NDC size of a guest pixel, which the line geometry
  // shader also uses to expand resolution-scaled lines to 1 guest pixel wide.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineStrip ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kLineLoop) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] =
        float(draw_resolution_scale_x) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    sc.point_screen_diameter_to_ndc_radius[1] =
        float(draw_resolution_scale_y) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
  }

  // Alpha test reference and alpha to coverage.
  sc.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                         ? (rb_colorcontrol.value >> 24) | (1 << 8)
                         : 0;

  // Color exponent bias (host render target path).
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
    int32_t color_exp_bias = color_info.color_exp_bias;
    if ((color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
         color_info.color_format ==
             xenos::ColorRenderTargetFormat::k_16_16_16_16) &&
        render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
      // Remap from -32...32 to -1...1, getting the full range. In the FSI path
      // the EDRAM store handles the format, so the bias must not be applied.
      color_exp_bias -= 5;
    }
    sc.color_exp_bias[i] = xe::memory::Reinterpret<float>(
        int32_t(0x3F800000 + (color_exp_bias << 23)));
  }

  // Host depth polygon offset (applied in the pixel shader when depth is not
  // written by the guest shader). FSI uses the EDRAM polygon offset written by
  // WriteFragmentShaderInterlockSystemConstants below instead.
  if (!edram_fsi && host_depth_polygon_offset) {
    float scale_factor =
        float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    sc.edram_poly_offset_front_scale =
        host_depth_polygon_offset->front_scale * scale_factor;
    sc.edram_poly_offset_front_offset = host_depth_polygon_offset->front_offset;
    sc.edram_poly_offset_back_scale =
        host_depth_polygon_offset->back_scale * scale_factor;
    sc.edram_poly_offset_back_offset = host_depth_polygon_offset->back_offset;
  }

  // Texture signedness. The SPIR-V pixel shader reads texture_swizzled_signs to
  // pick the signed or unsigned SRV variant and to decode each component. Left
  // zero, every fetch samples the unsigned variant - which is a null descriptor
  // for textures the cache only allocated a signed variant of (e.g. resolved
  // render targets), sampling black. The host swizzle is baked into the D3D12
  // SRV (image_view_format_swizzle is set), so texture_swizzles stays zero.
  // The used-texture mask comes from the guest shaders' bindings, gathered
  // during Mesa SPIR-V translation. Only read once published (bindings_ready).
  uint32_t used_texture_mask = 0;
  if (vertex_shader->bindings_ready()) {
    used_texture_mask |= vertex_shader->GetUsedTextureMaskAfterTranslation();
  }
  if (pixel_shader && pixel_shader->bindings_ready()) {
    used_texture_mask |= pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      sc.texture_swizzled_signs[texture_index >> 2] |=
          uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(texture_index))
          << (8 * (texture_index & 3));
      sc.texture_integer_scale_bits[texture_index] =
          texture_cache_->GetActiveIntegerScaleBits(texture_index);
    }
  }

  // Which textures come from a resolution-scaled resolve (only consumed by the
  // shader when the draw resolution scale is above 1).
  {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    sc.textures_resolved = textures_resolved;
  }

  // Fragment shader interlock (EDRAM ROP) flag bits and EDRAM constants, shared
  // with the Vulkan backend. The per field dirty flag is unused here. System
  // constant changes are detected by comparing the assembled buffer below.
  if (edram_fsi) {
    // Select the active occlusion query's counter slot so the FSI pixel shader
    // accumulates passed samples into it, UINT32_MAX outside a query.
    uint32_t zpd_fsi_counter_index = UINT32_MAX;
    if (zpd_active_query_is_rov_ && zpd_active_query_index_ != UINT32_MAX &&
        zpd_host_query_pool_->counter_initialized()) {
      zpd_fsi_counter_index = zpd_active_query_index_;
    }
    bool fsi_dirty = false;
    WriteFragmentShaderInterlockSystemConstants(
        sc, sc.flags, fsi_dirty, regs, primitive_polygonal,
        normalized_depth_control, normalized_color_mask,
        draw_resolution_scale_x, draw_resolution_scale_y,
        zpd_fsi_counter_index);
  } else {
    // Hybrid queries on the host render target path count pre-test coverage
    // into the same counter slot from the pixel shader.
    uint32_t zpd_counter_index = UINT32_MAX;
    if (zpd_active_segment_.hybrid && zpd_active_query_index_ != UINT32_MAX &&
        zpd_host_query_pool_->counter_initialized()) {
      zpd_counter_index = zpd_active_query_index_;
    }
    sc.zpd_fsi_counter_index = zpd_counter_index;
  }

  // Upload the constant buffers, skipping any whose data is unchanged since the
  // last draw. up_to_date is invalidated on the relevant register writes and at
  // submission start. The address is cached in the binding. The float, bool and
  // fetch buffers hold raw guest register data, packed here directly.

  // System constants are derived, not raw registers, so there is no per write
  // invalidation. Compare against the last uploaded copy instead.
  if (mesa_system_constants_shadow_.size() != sizeof(sc) ||
      std::memcmp(mesa_system_constants_shadow_.data(), &sc, sizeof(sc)) != 0) {
    mesa_system_constants_shadow_.assign(
        reinterpret_cast<const uint8_t*>(&sc),
        reinterpret_cast<const uint8_t*>(&sc) + sizeof(sc));
    cbuffer_binding_system_.up_to_date = false;
  }
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* mapping = constant_buffer_pool_->Request(
        frame_current_, sizeof(sc),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_system_.address);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, &sc, sizeof(sc));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(uint32_t(1) << kRootParameter_Mesa_SystemConstants);
  }

  // Invalidate the float constant buffers when the shader's used constant
  // layout changes, since a reused buffer would have the wrong size and
  // packing. An empty layout can reuse any buffer, so it is not invalidated.
  const Shader::ConstantRegisterMap& float_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_count_vertex = float_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] !=
        float_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_map_vertex.float_bitmap[i];
      if (float_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  // The interpreter placeholder needs the full 256 float4 register file (it
  // indexes every constant by raw register index), a different layout than the
  // packed subset, so invalidate the buffer when switching between them.
  if (interpreter_placeholder != mesa_float_constants_vertex_full_) {
    mesa_float_constants_vertex_full_ = interpreter_placeholder;
    cbuffer_binding_float_vertex_.up_to_date = false;
  }
  uint32_t float_count_pixel = 0;
  if (pixel_shader) {
    const Shader::ConstantRegisterMap& float_map_pixel =
        pixel_shader->constant_register_map();
    float_count_pixel = float_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] !=
          float_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_map_pixel.float_bitmap[i];
        if (float_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
  }

  if (!cbuffer_binding_float_vertex_.up_to_date) {
    uint32_t upload_count =
        interpreter_placeholder ? 256u : std::max(float_count_vertex, 1u);
    uint8_t* mapping = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * upload_count,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (!mapping) {
      return false;
    }
    if (interpreter_placeholder) {
      // The interpreter indexes all 256 float4 constants by raw register index,
      // so upload the whole contiguous register file.
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X],
                  sizeof(float) * 4 * 256);
    } else {
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t entry = float_map_vertex.float_bitmap[i];
        uint32_t index;
        while (xe::bit_scan_forward(entry, &index)) {
          entry = xe::clear_lowest_bit(entry);
          std::memcpy(
              mapping,
              &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) + (index << 2)],
              4 * sizeof(float));
          mapping += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(uint32_t(1) << kRootParameter_Mesa_FloatConstantsVertex);
  }

  // Pixel float constants. Depth-only draws have no pixel shader. The bound
  // host depth pixel shader reads none, so a minimal buffer keeps the CBV
  // valid.
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* mapping = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_count_pixel, 1u),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (!mapping) {
      return false;
    }
    if (pixel_shader) {
      const Shader::ConstantRegisterMap& float_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t entry = float_map_pixel.float_bitmap[i];
        uint32_t index;
        while (xe::bit_scan_forward(entry, &index)) {
          entry = xe::clear_lowest_bit(entry);
          std::memcpy(
              mapping,
              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) + (index << 2)],
              4 * sizeof(float));
          mapping += 4 * sizeof(float);
        }
      }
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(uint32_t(1) << kRootParameter_Mesa_FloatConstantsPixel);
  }

  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* mapping = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_bool_loop_.address);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(uint32_t(1) << kRootParameter_Mesa_BoolLoopConstants);
  }

  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* mapping = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_fetch_.address);
    if (!mapping) {
      return false;
    }
    std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(uint32_t(1) << kRootParameter_Mesa_FetchConstants);
  }

  // A small zeroed buffer for the unused Dozen runtime data CBV (b0, space31),
  // present only to give the root parameter a valid binding. Constant, so it is
  // uploaded once per submission.
  if (!cbuffer_binding_runtime_data_.up_to_date) {
    constexpr uint32_t kRuntimeDataSize = 256;
    uint8_t* mapping = constant_buffer_pool_->Request(
        frame_current_, kRuntimeDataSize,
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_runtime_data_.address);
    if (!mapping) {
      return false;
    }
    std::memset(mapping, 0, kRuntimeDataSize);
    cbuffer_binding_runtime_data_.up_to_date = true;
    current_graphics_root_up_to_date_ &=
        ~(uint32_t(1) << kRootParameter_Mesa_RuntimeData);
  }

  // Bind the constant buffer views, skipping any already bound with the current
  // address. The bit is cleared above when the buffer was re-uploaded, and when
  // the root signature changes.
  auto bind_cbv = [&](uint32_t root_parameter,
                      const ConstantBufferBinding& binding) {
    uint32_t bit = uint32_t(1) << root_parameter;
    if (!(current_graphics_root_up_to_date_ & bit)) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          root_parameter, binding.address);
      current_graphics_root_up_to_date_ |= bit;
    }
  };
  bind_cbv(kRootParameter_Mesa_SystemConstants, cbuffer_binding_system_);
  bind_cbv(kRootParameter_Mesa_FloatConstantsVertex,
           cbuffer_binding_float_vertex_);
  bind_cbv(kRootParameter_Mesa_FloatConstantsPixel,
           cbuffer_binding_float_pixel_);
  bind_cbv(kRootParameter_Mesa_BoolLoopConstants, cbuffer_binding_bool_loop_);
  bind_cbv(kRootParameter_Mesa_FetchConstants, cbuffer_binding_fetch_);
  bind_cbv(kRootParameter_Mesa_RuntimeData, cbuffer_binding_runtime_data_);

  // Shared memory descriptor table (SRV t0 + UAV u0, space0). The bindless
  // system view heap holds an adjacent [shared SRV, null UAV] pair for reads
  // and [null SRV, shared UAV] pair for memexport writes, matching the table
  // layout. Memexport-routed draws pick the host buffer's equivalent pairs.
  SystemBindlessView shared_memory_view;
  if (route_to_host) {
    // A memexport draw also reads its vertices through the t0 SRV, so bind the
    // host buffer as both SRV and UAV.
    shared_memory_view =
        memexport_used
            ? SystemBindlessView::kSharedMemoryHostRawSRVAndHostRawUAVStart
            : SystemBindlessView::kSharedMemoryHostRawSRVAndNullRawUAVStart;
  } else {
    // Same as the host path. Bind the device buffer as both SRV and UAV.
    shared_memory_view =
        memexport_used
            ? SystemBindlessView::kSharedMemoryRawSRVAndRawUAVStart
            : SystemBindlessView::kSharedMemoryRawSRVAndNullRawUAVStart;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE shared_memory_handle =
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_,
                                    uint32_t(shared_memory_view));
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      kRootParameter_Mesa_SharedMemory, shared_memory_handle);

  // ZPD counter (u1) and EDRAM (u2) UAVs: the pixel shaders declare them at
  // SPIR-V set 0 bindings 1 and 2. The counter is also used by RTV hybrid
  // queries, and its descriptor is a null UAV while no counter exists, so it
  // is always bound. EDRAM only exists in the pixel shader interlock path, and
  // host render target shaders never reference that root parameter.
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      kRootParameter_Mesa_ZpdCounter,
      provider.OffsetViewDescriptor(
          view_bindless_heap_gpu_start_,
          uint32_t(SystemBindlessView::kZpdCounterRawUAV)));
  if (render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock) {
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
        kRootParameter_Mesa_Edram,
        provider.OffsetViewDescriptor(
            view_bindless_heap_gpu_start_,
            uint32_t(SystemBindlessView::kEdramRawUAV)));
  }

  // Per-stage bindless texture/sampler index buffers (raw buffers of
  // {texture_idx, sampler_idx} entries, one per SPIR-V binding). The Mesa DXIL
  // reads a heap index from these and indexes ResourceDescriptorHeap /
  // SamplerDescriptorHeap. Bound even when empty so the root SRV is valid.
  bool sampler_overflow = false;
  auto bind_index_buffer = [&](const SpirvShader* shader,
                               uint32_t root_parameter) -> bool {
    // The index buffer follows the SpirvShader's binding order (which the Mesa
    // DXIL was generated from). Each binding's heap index is resolved directly
    // from its fetch constant / dimension / sign via the texture cache (k1D and
    // k2D resolve identically in the cache, so the SPIR-V translator's 1D-as-2D
    // is fine).
    size_t texture_count = 0, sampler_count = 0;
    const std::vector<SpirvShader::TextureBinding>* spirv_textures = nullptr;
    const std::vector<SpirvShader::SamplerBinding>* spirv_samplers = nullptr;
    // Only read the bindings once published (bindings_ready), so the deferred
    // pixel shader's still-translating bindings are never read partially.
    const SpirvShader* spirv_shader =
        (shader && shader->bindings_ready()) ? shader : nullptr;
    if (spirv_shader) {
      spirv_textures = &spirv_shader->GetTextureBindingsAfterTranslation();
      spirv_samplers = &spirv_shader->GetSamplerBindingsAfterTranslation();
      texture_count = spirv_textures->size();
      sampler_count = spirv_samplers->size();
    }
    size_t entry_count = texture_count + sampler_count;
    // 2 uint32 per entry: [0] = texture heap index, [1] = sampler heap index.
    size_t buffer_size =
        std::max(entry_count, size_t(1)) * 2 * sizeof(uint32_t);
    D3D12_GPU_VIRTUAL_ADDRESS address;
    uint32_t* mapping =
        reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
            frame_current_, buffer_size, D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT,
            nullptr, nullptr, &address));
    if (!mapping) {
      return false;
    }
    std::memset(mapping, 0, buffer_size);
    for (size_t i = 0; i < texture_count; ++i) {
      // ResourceDescriptorHeap is the bound view heap, so the absolute index is
      // used (no SystemBindlessView::kUnboundedSRVsStart subtraction).
      mapping[i * 2] = texture_cache_->GetActiveTextureBindlessSRVIndex(
          (*spirv_textures)[i]);
    }
    for (size_t j = 0; j < sampler_count; ++j) {
      uint32_t sampler_index = GetOrCreateMesaBindlessSamplerIndex(
          texture_cache_->GetSamplerParameters((*spirv_samplers)[j]));
      if (sampler_index == UINT32_MAX) {
        // Heap full. Stop and let the caller switch heaps and rebuild.
        sampler_overflow = true;
        break;
      }
      // Samplers follow textures in the SPIR-V binding space (binding
      // texture_count + j). The index is the second uint32 of the entry.
      mapping[(texture_count + j) * 2 + 1] = sampler_index;
    }
    deferred_command_list_.D3DSetGraphicsRootShaderResourceView(root_parameter,
                                                                address);
    return true;
  };
  // Build both stages' index buffers, resolving sampler heap indices. If the
  // sampler heap overflows, switch to a fresh heap and rebuild both buffers,
  // since switching clears the sampler map. A single draw's samplers always fit
  // a fresh heap, so one switch is enough.
  for (uint32_t attempt = 0;; ++attempt) {
    sampler_overflow = false;
    if (!bind_index_buffer(vertex_shader,
                           kRootParameter_Mesa_VertexTextureIndices) ||
        !bind_index_buffer(pixel_shader,
                           kRootParameter_Mesa_PixelTextureIndices)) {
      return false;
    }
    if (!sampler_overflow) {
      break;
    }
    if (attempt != 0 || !SwitchToNewBindlessSamplerHeap()) {
      return false;
    }
  }

  // Satisfy the vestigial texture/sampler declarations left by the bindless
  // lowering: point their (never-dereferenced) ranges at the bindless heaps.
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      kRootParameter_Mesa_VertexTextureRange, view_bindless_heap_gpu_start_);
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      kRootParameter_Mesa_PixelTextureRange, view_bindless_heap_gpu_start_);
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      kRootParameter_Mesa_VertexSamplerRange, sampler_bindless_heap_gpu_start_);
  deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
      kRootParameter_Mesa_PixelSamplerRange, sampler_bindless_heap_gpu_start_);

  return true;
}

uint32_t D3D12CommandProcessor::GetOrCreateMesaBindlessSamplerIndex(
    D3D12TextureCache::SamplerParameters parameters) {
  auto it = texture_cache_bindless_sampler_map_.find(parameters.value);
  if (it != texture_cache_bindless_sampler_map_.end()) {
    return it->second;
  }
  if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
    return UINT32_MAX;
  }
  uint32_t sampler_index = sampler_bindless_heap_allocated_++;
  texture_cache_->WriteSampler(
      parameters, GetD3D12Provider().OffsetSamplerDescriptor(
                      sampler_bindless_heap_cpu_start_, sampler_index));
  texture_cache_bindless_sampler_map_.emplace(parameters.value, sampler_index);
  return sampler_index;
}

bool D3D12CommandProcessor::SwitchToNewBindlessSamplerHeap() {
  // Reuse the oldest retired heap once the GPU is done with it, otherwise
  // create a new one. Retire the current heap until its submission completes.
  ID3D12DescriptorHeap* sampler_heap_new;
  if (!sampler_bindless_heaps_overflowed_.empty() &&
      sampler_bindless_heaps_overflowed_.front().second <=
          GetCompletedSubmission()) {
    sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
    sampler_bindless_heaps_overflowed_.pop_front();
  } else {
    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
    sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
    sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_heap_new_desc.NodeMask = 0;
    if (FAILED(GetD3D12Provider().GetDevice()->CreateDescriptorHeap(
            &sampler_heap_new_desc, IID_PPV_ARGS(&sampler_heap_new)))) {
      XELOGE(
          "Failed to create a new bindless sampler descriptor heap after an "
          "overflow of the previous one");
      return false;
    }
  }
  sampler_bindless_heaps_overflowed_.push_back(
      std::make_pair(sampler_bindless_heap_current_, GetCurrentSubmission()));
  sampler_bindless_heap_current_ = sampler_heap_new;
  sampler_bindless_heap_cpu_start_ =
      sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
  sampler_bindless_heap_gpu_start_ =
      sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
  sampler_bindless_heap_allocated_ = 0;
  // The heap only holds texture cache samplers, so invalidate all of them.
  texture_cache_bindless_sampler_map_.clear();
  deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                            sampler_bindless_heap_current_);
  return true;
}

void D3D12CommandProcessor::EnsureZPDQueryResources() {
  if (zpd_mode_ == ZPDMode::kFake || !zpd_host_query_pool_) {
    return;
  }

  bool can_recreate = zpd_current_report_.handle == kInvalidReportHandle &&
                      !zpd_active_segment_.segment_active &&
                      zpd_active_query_index_ == UINT32_MAX &&
                      !zpd_active_query_is_rov_ &&
                      !zpd_host_query_pool_->has_pending_resolve_batch() &&
                      zpd_resolves_in_flight_.empty();
  bool rov_path = render_target_cache_ &&
                  render_target_cache_->GetPath() ==
                      RenderTargetCache::Path::kPixelShaderInterlock;
  zpd_rov_path_ = rov_path;
  // The counter clear uses WriteBufferImmediate, so only initialize when
  // CommandList2 is available.
  bool can_initialize_counter =
      (rov_path || zpd_hybrid_supported_) && command_list_2_;

  ID3D12Resource* counter_buffer = nullptr;
  uint32_t counter_capacity = 0;
  if (zpd_host_query_pool_->EnsureInitialized(
          GetD3D12Provider(), kZPDQueryPoolCapacity, can_recreate,
          can_initialize_counter) &&
      zpd_host_query_pool_->counter_initialized()) {
    counter_buffer = zpd_host_query_pool_->counter_buffer();
    counter_capacity = zpd_host_query_pool_->capacity();
  }
  if (bindless_resources_used_) {
    WriteZPDCounterRawUAVDescriptor(GetD3D12Provider().OffsetViewDescriptor(
        view_bindless_heap_cpu_start_,
        uint32_t(SystemBindlessView::kZpdCounterRawUAV)));
  } else if (bindful_zpd_counter_buffer_ != counter_buffer ||
             bindful_zpd_counter_capacity_ != counter_capacity) {
    // If the counter appears or changes after a bindful page was built,
    // then an old page can end up counting into a null/stale UAV. So invalidate
    // it and let the normal bindful rebuild pick up the current counter.
    bindful_zpd_counter_buffer_ = counter_buffer;
    bindful_zpd_counter_capacity_ = counter_capacity;
    draw_view_bindful_heap_index_ =
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  if (zpd_rov_path_ && !IsZPDQueryPoolReady()) {
    if (!command_list_2_) {
      XELOGW(
          "ZPD/D3D12: Counter unavailable because CommandList2 is not "
          "available; keeping counter index sentinel active");
    } else {
      XELOGW(
          "ZPD/D3D12: Counter resources unavailable; keeping counter index "
          "sentinel active");
    }
  }
}

bool D3D12CommandProcessor::IsZPDQueryPoolReady() const {
  if (!zpd_host_query_pool_ || !zpd_host_query_pool_->rtv_initialized()) {
    return false;
  }
  if (!zpd_rov_path_) {
    return true;
  }
  return zpd_host_query_pool_->counter_initialized();
}

bool D3D12CommandProcessor::CanOpenZPDQuery() const { return submission_open_; }

CommandProcessor::QueryOpenResult D3D12CommandProcessor::OpenZPDQuery(
    bool can_close_submission) {
  bool use_rov_path =
      zpd_rov_path_ && zpd_host_query_pool_->counter_initialized();
  bool is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();

  if (is_pool_exhausted) {
    PumpQueryResolves();
    is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();
  }

  if (is_pool_exhausted) {
    if (zpd_mode_ != ZPDMode::kStrict) {
      return QueryOpenResult::kPoolExhausted;
    }

    // Strict mode can't guess, so wait for the oldest in-flight resolve to
    // hand a slot back. If it's still in the open submission, close that when
    // allowed and let the next draw retry.
    uint64_t wait_for = 0;
    if (!zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (wait_for >= GetCurrentSubmission()) {
        if (can_close_submission) {
          if (!EndSubmission(false)) {
            return QueryOpenResult::kFailed;
          }
        }
        return QueryOpenResult::kDeferred;
      }

      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }

      completion_timeline_->AwaitSubmissionAndUpdateCompleted(wait_for);
      PumpQueryResolves();
      is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();
    }
  }

  if (is_pool_exhausted) {
    return QueryOpenResult::kDeferred;
  }

  if (!zpd_host_query_pool_->AcquireQueryIndex(zpd_active_query_index_,
                                               zpd_active_query_generation_)) {
    return QueryOpenResult::kFailed;
  }

  zpd_active_query_is_rov_ = use_rov_path;
  zpd_active_segment_.hybrid = !use_rov_path && zpd_hybrid_supported_ &&
                               zpd_active_segment_.count_total &&
                               zpd_host_query_pool_->counter_initialized();

  // ROV queries don't use D3D12 occlusion queries at all, and hybrid queries
  // only use them for ZPass. While the segment is open, the translated pixel
  // shader accumulates depth/stencil testing into one counter slot selected
  // via zpd_counter_index. Clear the slot here so a recycled index never
  // inherits old counts.
  if (zpd_active_query_is_rov_ || zpd_active_segment_.hybrid) {
    zpd_host_query_pool_->ClearCounter(deferred_command_list_,
                                       GetCurrentSubmission(),
                                       zpd_active_query_index_);
    if (zpd_active_query_is_rov_) {
      return QueryOpenResult::kOpened;
    }
  }

  zpd_host_query_pool_->BeginQuery(deferred_command_list_,
                                   zpd_active_query_index_);
  return QueryOpenResult::kOpened;
}

bool D3D12CommandProcessor::CloseZPDQuery(ReportHandle report_handle,
                                          uint64_t& out_submission) {
  if (zpd_active_query_is_rov_) {
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, true);
  } else {
    zpd_host_query_pool_->EndQuery(deferred_command_list_,
                                   zpd_active_query_index_);
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, false);
    if (zpd_active_segment_.hybrid) {
      zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, true);
    }
  }

  PendingQueryResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.query_index = zpd_active_query_index_;
  resolve.query_generation = zpd_active_query_generation_;
  resolve.scale_area = GetZPDScaleArea();
  resolve.rov = zpd_active_query_is_rov_;
  resolve.hybrid = zpd_active_segment_.hybrid;
  resolve.report_handle = report_handle;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_index_ = UINT32_MAX;
  zpd_active_query_generation_ = 0;
  zpd_active_query_is_rov_ = false;
  return true;
}

void D3D12CommandProcessor::PumpQueryResolves() {
  if (!zpd_host_query_pool_ || zpd_resolves_in_flight_.empty()) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  while (!zpd_resolves_in_flight_.empty() &&
         zpd_resolves_in_flight_.front().submission <= completed) {
    PendingQueryResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (zpd_host_query_pool_->GenerationMatches(resolve.query_index,
                                                resolve.query_generation)) {
      XenosZPDReport raw_counts = zpd_host_query_pool_->GetQueryReadbackValue(
          resolve.query_index, resolve.rov, resolve.hybrid);
      zpd_host_query_pool_->ReleaseQueryIndex(resolve.query_index,
                                              resolve.query_generation);
      OnZPDQueryResolved(resolve.report_handle, raw_counts, resolve.scale_area);
    } else {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/D3D12: Dropping stale query index={} generation={} "
            "handle={}",
            resolve.query_index, resolve.query_generation,
            resolve.report_handle);
      }
    }
  }
}

bool D3D12CommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                              uint64_t wait_for_submission) {
  assert_not_zero(wait_for_submission);

  // Resolve is still pending. Wait for async pipeline creation to finish.
  if (wait_for_submission >= GetCurrentSubmission()) {
    if (!submission_open_) {
      return false;
    }
    if (!CanEndSubmissionImmediately()) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD: Awaiting pending D3D12 pipeline for active query retirement");
      }
      pipeline_cache_->AwaitPipelineCompletion();
    }
    if (!CanEndSubmissionImmediately() || !EndSubmission(false)) {
      return false;
    }
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    completion_timeline_->AwaitSubmissionAndUpdateCompleted(
        wait_for_submission);
  }

  PumpQueryResolves();

  const ZPDReport* report = FindZPDReport(report_handle);
  return !report || !report->pending_segments;
}

void D3D12CommandProcessor::RecordZPDResolveBatch() {
  zpd_host_query_pool_->FlushResolveBatch(
      deferred_command_list_, GetCurrentSubmission(), submission_open_);
}

void D3D12CommandProcessor::WriteGammaRampSRV(
    bool is_pwl, D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

void D3D12CommandProcessor::WriteZPDCounterRawUAVDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (zpd_host_query_pool_ && zpd_host_query_pool_->counter_initialized()) {
    ui::d3d12::util::CreateBufferRawUAV(
        device, handle, zpd_host_query_pool_->counter_buffer(),
        XenosZPDReport::kCounterSizeBytes * zpd_host_query_pool_->capacity());
    return;
  }

  ui::d3d12::util::CreateBufferRawUAV(device, handle, nullptr, 0);
}

#define COMMAND_PROCESSOR D3D12CommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR
}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
