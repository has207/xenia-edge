/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamMediaVerificationCreate_entry(dword_t creation_flags) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationCreate, kNone, kStub);

dword_result_t XamMediaVerificationClose_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamMediaVerificationClose, kNone, kStub);

dword_result_t XamMediaVerificationVerify_entry(
    dword_t r3, pointer_t<XAM_OVERLAPPED> overlapped, lpvoid_t callback) {
  if (callback) {
    auto thread = XThread::GetCurrentThread();
    thread->EnqueueApc(static_cast<uint32_t>(callback) & ~1u, 0, 0, 0);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationVerify, kNone, kStub);

struct X_SECURITY_FAILURE_INFORMATION {
  xe::be<uint32_t> size;
  xe::be<uint32_t> failed_reads;
  xe::be<uint32_t> failed_hashes;
  xe::be<uint32_t> blocks_checked;
  xe::be<uint32_t> total_blocks;
  xe::be<uint32_t> complete;
};

dword_result_t XamMediaVerificationFailedBlocks_entry(
    pointer_t<X_SECURITY_FAILURE_INFORMATION> failure_information) {
  if (failure_information) {
    if (failure_information->size != sizeof(X_SECURITY_FAILURE_INFORMATION)) {
      return X_ERROR_NOT_ENOUGH_MEMORY;
    }

    failure_information->failed_reads = 0;
    failure_information->failed_hashes = 0;
    failure_information->blocks_checked = 0;
    failure_information->total_blocks = 0;
    failure_information->complete = true;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationFailedBlocks, kNone, kStub);

dword_result_t XamMediaVerificationInject_entry(dword_t r3, dword_t r4) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationInject, kNone, kStub);

// xam keeps the closed captioning switch and style in two console config
// settings that xenia's config doesn't have, so they live here. Captions start
// off with the default style.
static std::atomic<uint8_t> closed_captioning_enabled_ = 0;
static std::atomic<uint32_t> closed_captioning_settings_ = 0;

dword_result_t XamGetClosedCaptioningState_entry() {
  return closed_captioning_enabled_.load();
}
DECLARE_XAM_EXPORT1(XamGetClosedCaptioningState, kNone, kImplemented);

void XamSetClosedCaptioningState_entry(dword_t enabled) {
  closed_captioning_enabled_ = static_cast<uint8_t>(enabled);
}
DECLARE_XAM_EXPORT1(XamSetClosedCaptioningState, kNone, kImplemented);

dword_result_t XamGetClosedCaptioningSettings_entry(lpdword_t settings_ptr) {
  if (!settings_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  *settings_ptr = closed_captioning_settings_.load();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetClosedCaptioningSettings, kNone, kImplemented);

// The extra AV codecs are system modules that xam loads on request and counts
// references to. The handle it returns is the module's entry point. Their
// images aren't available, so loading succeeds with no module, which npctrl
// takes as there being no extra codecs. It doesn't recover from a failure.
constexpr X_HRESULT kErrorPointer = 0x80004003;

// Two codec modules share one loader: ordinals 0x7DC/0x7DD load and unload the
// first, 0x7E3/0x7E4 the second.
static dword_result_t LoadExtraAVCodecs(lpdword_t module_handle_ptr) {
  if (!module_handle_ptr) {
    return kErrorPointer;
  }
  *module_handle_ptr = 0;
  return X_E_SUCCESS;
}

dword_result_t XamLoadExtraAVCodecs_entry(lpdword_t module_handle_ptr) {
  return LoadExtraAVCodecs(module_handle_ptr);
}
DECLARE_XAM_EXPORT1(XamLoadExtraAVCodecs, kNone, kStub);

void XamUnloadExtraAVCodecs_entry() {
  // Nothing was loaded.
}
DECLARE_XAM_EXPORT1(XamUnloadExtraAVCodecs, kNone, kStub);

dword_result_t XamLoadExtraAVCodecs2_entry(lpdword_t module_handle_ptr) {
  return LoadExtraAVCodecs(module_handle_ptr);
}
DECLARE_XAM_EXPORT1(XamLoadExtraAVCodecs2, kNone, kStub);

void XamUnloadExtraAVCodecs2_entry() {
  // Nothing was loaded.
}
DECLARE_XAM_EXPORT1(XamUnloadExtraAVCodecs2, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Media);
