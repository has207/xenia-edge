/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/backend.h"

#include <cstring>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"

DEFINE_bool(debugprint_trap_log, false,
            "Log debugprint traps to the active debugger", "CPU");

namespace xe {
namespace cpu {
namespace backend {

Backend::Backend() { std::memset(&machine_info_, 0, sizeof(machine_info_)); }
Backend::~Backend() = default;

bool Backend::Initialize(Processor* processor) {
  processor_ = processor;
  return true;
}

void* Backend::AllocThreadData() { return nullptr; }

void Backend::FreeThreadData(void* thread_data) {}

void (*preempt_yield_handler)(void* raw_context) = nullptr;

uint64_t TrapDebugPrint(void* raw_context) {
  auto thread_state =
      reinterpret_cast<ppc::PPCContext_s*>(raw_context)->thread_state;
  uint32_t str_ptr = uint32_t(thread_state->context()->r[3]);
  uint32_t str_length = uint32_t(thread_state->context()->r[4]);
  auto str = thread_state->memory()->TranslateVirtual<const char*>(str_ptr);
  std::string message(str, str_length);
  XELOGD("(DebugPrint) {}", message);
  if (cvars::debugprint_trap_log) {
    debugging::DebugPrint("(DebugPrint) {}", message);
  }
  return 0;
}

uint32_t Backend::ReservedLoad32(ppc::PPCContext* context, uint32_t address) {
  return xe::byte_swap(*context->TranslateVirtual<uint32_t*>(address));
}

uint64_t Backend::ReservedLoad64(ppc::PPCContext* context, uint32_t address) {
  return xe::byte_swap(*context->TranslateVirtual<uint64_t*>(address));
}

bool Backend::ReservedStore32(ppc::PPCContext* context, uint32_t address,
                              uint32_t value) {
  *context->TranslateVirtual<uint32_t*>(address) = xe::byte_swap(value);
  return true;
}

bool Backend::ReservedStore64(ppc::PPCContext* context, uint32_t address,
                              uint64_t value) {
  *context->TranslateVirtual<uint64_t*>(address) = xe::byte_swap(value);
  return true;
}

}  // namespace backend
}  // namespace cpu
}  // namespace xe
