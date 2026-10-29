/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>

#include "xenia/base/math.h"
#include "xenia/cpu/lzx.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DEFINE_bool(allow_mic_initialization, false,
            "Enable Mic Initialization\n"
            " Only set true when testing mic related functions",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

enum class XMicRequestType : uint16_t {
  MicGetStatus = 0x0000,
  MicStart = 0x0001,
  MicSetGain = 0x0002,
  MicGetData = 0x0007,
  MicGetSampleRates = 0x0008,
  MicGetCapabilities = 0x0009,
  MicUnk = 0x000B,
};

enum class XMicState : uint32_t {
  MicNotConnected = 0x00000000,
  MicConnected = 0x00000001,
  MicConnected2 = 0x00000002,
  MicConnected3 = 0x00000003,
  MicRecording = 0x00000005,
};

struct X_MIC_INFO {
  xe::be<XMicRequestType> request_type;  // 0
  xe::be<uint16_t> user_index;           // 2
  xe::be<XMicState> state;               // 4

  // These fields are only for requests that use overlap. Basically anything
  // other than MicGetStatus. Seems like there is also an event object which
  // creation isn't reported via kernel call.
  xe::be<uint32_t> unk1;            // 8
  xe::be<uint32_t> event_ptr;       // 0xC
  xe::be<uint32_t> completion_ptr;  // 0x10
  xe::be<uint32_t> unk4;            // 0x14
};

struct X_MIC_CAPABILITIES {
  /* Hardcoded fields for 0x6000 type aka. MicDevice. 0x5000 type (Rmc) does not
    have any hardcoded fields. features - 0 frame_length - 0x3E8 mic_color -
    0xFF
  */
  xe::be<uint32_t> features;
  xe::be<uint16_t> format_tag;  // Always 1
  xe::be<uint16_t> channels;
  xe::be<uint32_t> sample_rates;
  xe::be<uint16_t> bits_per_sample;
  xe::be<uint16_t> frame_length;
  xe::be<uint8_t> mic_color;
  xe::be<uint16_t> vendor_id;
  xe::be<uint16_t> product_id;
  xe::be<uint16_t> revision;
  xe::be<uint32_t> device_id;
};
static_assert_size(X_MIC_CAPABILITIES, 0x1C);

struct X_MIC_DEVICE {
  X_MIC_INFO info;
  // There seems to be one huge union here that depends on the request type.
  X_MIC_CAPABILITIES capabilities;
};

void KeEnableFpuExceptions_entry(
    const ppc_context_t& ctx) {  // dword_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
  // theres a lot more thats supposed to happen here, the floating point state
  // has to be saved to kthread, the irql changes, the machine state register is
  // changed to enable exceptions

  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);
  kthread->fpu_exceptions_on = static_cast<uint32_t>(ctx->r[3]) != 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeEnableFpuExceptions, kNone, kStub);

void KeSaveFloatingPointState_entry(const ppc_context_t& ctx) {
  // Probably we should use: thread_fpu_related to store/restore state
  X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    kthread->fpu_context[i] = ctx->f[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeSaveFloatingPointState, kNone, kSketchy);

void KeRestoreFloatingPointState_entry(const ppc_context_t& ctx) {
  const X_KTHREAD* kthread = ctx->TranslateVirtual(
      ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13])->prcb_data.current_thread);

  for (size_t i = 0; i < xe::countof(ctx->f); ++i) {
    ctx->f[i] = kthread->fpu_context[i];
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeRestoreFloatingPointState, kNone, kSketchy);

static qword_result_t KeQueryInterruptTime_entry(const ppc_context_t& ctx) {
  auto kstate = ctx->kernel_state;
  uint32_t ts_bundle = kstate->GetKeTimestampBundle();
  X_TIME_STAMP_BUNDLE* bundle =
      ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle);

  return xe::load_and_swap<uint64_t>(&bundle->interrupt_time);
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryInterruptTime, kNone, kImplemented);

dword_result_t MicDeviceRequest_entry(pointer_t<X_MIC_DEVICE> device_ptr) {
  if (!device_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  XELOGE("MicDeviceRequest State: {:08X} Action: {:04X} USER: {:08X}",
         static_cast<uint32_t>(device_ptr->info.state.get()),
         static_cast<uint16_t>(device_ptr->info.request_type.get()),
         device_ptr->info.user_index.get());
  if (device_ptr->info.user_index > XUserMaxUserCount) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (device_ptr->info.request_type > XMicRequestType::MicUnk) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }

  device_ptr->info.state = cvars::allow_mic_initialization
                               ? XMicState::MicConnected
                               : XMicState::MicNotConnected;

  switch (device_ptr->info.request_type) {
    case XMicRequestType::MicGetStatus:
      return X_ERROR_SUCCESS;
    case XMicRequestType::MicStart:
    case XMicRequestType::MicGetData:
    case XMicRequestType::MicGetSampleRates:
      break;
    case XMicRequestType::MicGetCapabilities:
      device_ptr->capabilities.features = 0x100;
      device_ptr->capabilities.format_tag = 1;
      device_ptr->capabilities.mic_color = 0;
      break;
    default:
      break;
  }

  // This should be handled by DPC.
  if (device_ptr->info.completion_ptr) {
    // Not sure what args should be put there, but seems like negative r4 causes
    // instant bailout. For now leave it hardcoded to failure.
    uint64_t args[] = {device_ptr.guest_address(),
                       static_cast<uint64_t>(X_STATUS_UNSUCCESSFUL)};

    kernel_state()->processor()->Execute(
        XThread::GetCurrentThread()->thread_state(),
        device_ptr->info.completion_ptr, args, xe::countof(args));
  }

  return X_STATUS_PENDING;
}
DECLARE_XBOXKRNL_EXPORT1(MicDeviceRequest, kNone, kStub);

constexpr X_STATUS kStatusDeviceNotConnected = 0xC000009D;

// The request block MicDeviceRequest takes, for the 0x5000 class of device.
// None is ever attached, so a status query says not connected and anything
// else the kernel would accept fails the same way.
dword_result_t RmcDeviceRequest_entry(pointer_t<X_MIC_INFO> request) {
  if (!request || request->user_index >= 4) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const auto request_type =
      static_cast<uint16_t>(XMicRequestType(request->request_type));
  if (request_type == uint16_t(XMicRequestType::MicGetStatus)) {
    request->state = XMicState::MicNotConnected;
    return X_STATUS_SUCCESS;
  }
  if (request_type > 10) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  return kStatusDeviceNotConnected;
}
DECLARE_XBOXKRNL_EXPORT1(RmcDeviceRequest, kNone, kImplemented);

// Event tracing. Nothing collects the events, so registering only does the
// kernel's checks and fills in the size, which is all unregistering looks at.
struct X_ETX_PRODUCER_REGISTRATION {
  xe::be<uint32_t> size;
};

dword_result_t EtxProducerRegister_entry(dword_t provider, lpvoid_t events,
                                         dword_t event_count, dword_t flags,
                                         dword_t registration_size,
                                         lpvoid_t registration) {
  if (registration_size < 0x40 || !registration) {
    return X_STATUS_INVALID_PARAMETER;
  }
  registration.Zero(registration_size);
  registration.as<X_ETX_PRODUCER_REGISTRATION*>()->size =
      uint32_t(registration_size);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerRegister, kNone, kStub);

dword_result_t EtxProducerUnregister_entry(
    pointer_t<X_ETX_PRODUCER_REGISTRATION> registration) {
  if (!registration || registration->size < 0x40) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerUnregister, kNone, kStub);

// Takes the registration, the event and a variable argument list for its
// payload, all of which go nowhere.
dword_result_t EtxProducerLog_entry(lpvoid_t registration, dword_t event) {
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(EtxProducerLog, kNone, kStub);

// The kernel's LZX decompression as cabinets use it. A context keeps the
// stream and each call passes one compressed frame.
//
// A context is a host object so its handle is checked and it goes with the
// object table. On the console it's a guest allocation that takes no handle.
class XLdiContext : public XObject {
 public:
  XLdiContext(KernelState* kernel_state,
              std::unique_ptr<LzxFrameDecoder> decoder)
      : XObject(kernel_state, Type::Undefined, true),
        decoder(std::move(decoder)) {}

  const std::unique_ptr<LzxFrameDecoder> decoder;
};

static object_ref<XObject> LookupLdiContext(uint32_t handle,
                                            XLdiContext** out_context) {
  auto object = kernel_state()->object_table()->LookupObject<XObject>(handle);
  *out_context = dynamic_cast<XLdiContext*>(object.get());
  return object;
}

constexpr uint32_t kLdiErrorNone = 0;
constexpr uint32_t kLdiErrorNotEnoughMemory = 1;
constexpr uint32_t kLdiErrorBadParameters = 2;
constexpr uint32_t kLdiErrorBufferOverflow = 3;
constexpr uint32_t kLdiErrorFailed = 4;
constexpr uint32_t kLdiErrorConfiguration = 5;
// An LZX frame inflates to 32 KB and can be up to 6 KB larger compressed.
constexpr uint32_t kLdiFrameSize = 0x8000;
constexpr uint32_t kLdiSourceBufferMin = kLdiFrameSize + 0x1800;

dword_result_t LDICreateDecompression_entry(
    lpdword_t data_block_max, lpdword_t configuration, dword_t alloc_function,
    dword_t free_function, dword_t unknown, lpdword_t source_buffer_min,
    lpdword_t handle_ptr) {
  // LZXDECOMPRESS: the window size, then the CPU type.
  if (!configuration || !handle_ptr) {
    return kLdiErrorBadParameters;
  }
  const uint32_t window_size = *configuration;
  if (!xe::is_pow2(window_size) || window_size < 0x8000 ||
      window_size > 0x200000) {
    return kLdiErrorConfiguration;
  }
  auto decoder = std::make_unique<LzxFrameDecoder>(window_size);
  if (!decoder->Reset()) {
    return kLdiErrorNotEnoughMemory;
  }
  // The creation reference passes to the handle.
  auto context = object_ref<XLdiContext>(
      new XLdiContext(kernel_state(), std::move(decoder)));
  *handle_ptr = context->handle();
  if (source_buffer_min) {
    *source_buffer_min = kLdiSourceBufferMin;
  }
  return kLdiErrorNone;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

dword_result_t LDIDecompress_entry(dword_t handle, lpvoid_t source,
                                   dword_t source_size, lpvoid_t dest,
                                   lpdword_t dest_size) {
  if (!source || !dest || !dest_size) {
    return kLdiErrorBadParameters;
  }
  XLdiContext* context;
  auto object = LookupLdiContext(handle, &context);
  if (!context) {
    return kLdiErrorBadParameters;
  }
  // The size the caller expects this frame to inflate to.
  const uint32_t frame_size = *dest_size;
  if (!frame_size || frame_size > kLdiFrameSize) {
    return kLdiErrorBufferOverflow;
  }
  if (source_size > kLdiSourceBufferMin) {
    return kLdiErrorBadParameters;
  }
  if (!context->decoder->Decompress(source.as<const uint8_t*>(), source_size,
                                    dest.as<uint8_t*>(), frame_size)) {
    return kLdiErrorFailed;
  }
  return kLdiErrorNone;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kImplemented);

dword_result_t LDIResetDecompression_entry(dword_t handle) {
  XLdiContext* context;
  auto object = LookupLdiContext(handle, &context);
  if (!context) {
    return kLdiErrorBadParameters;
  }
  if (!context->decoder->Reset()) {
    return kLdiErrorNotEnoughMemory;
  }
  return kLdiErrorNone;
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kImplemented);

dword_result_t LDIDestroyDecompression_entry(dword_t handle) {
  XLdiContext* context;
  auto object = LookupLdiContext(handle, &context);
  if (!context) {
    return kLdiErrorBadParameters;
  }
  context->ReleaseHandle();
  return kLdiErrorNone;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Misc);
