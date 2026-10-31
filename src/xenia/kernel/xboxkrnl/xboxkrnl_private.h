/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_PRIVATE_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_PRIVATE_H_

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ordinals.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

xe::cpu::Export* RegisterExport_xboxkrnl(xe::cpu::Export* export_entry);

// Delivers a guest call to an address with no code to the guest's frame-based
// exception handlers, as the console's kernel does for the instruction fetch
// fault the call raises there. Returns the address a handler continued at, with
// the context loaded from its CONTEXT record, or 0 if none did.
uint32_t DispatchCodeFetchFault(xe::cpu::ppc::PPCContext* context,
                                uint32_t address);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                  \
  void Register##n##Exports(xe::cpu::ExportResolver* export_resolver, \
                            KernelState* kernel_state);
#include "xboxkrnl_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_PRIVATE_H_
