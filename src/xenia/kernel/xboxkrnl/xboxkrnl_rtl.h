/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_RTL_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_RTL_H_

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Guest CONTEXT record fields.
constexpr uint32_t kContextFlags = 0x0;
constexpr uint32_t kContextMsr = 0x4;
constexpr uint32_t kContextIar = 0x8;
constexpr uint32_t kContextLr = 0xC;
constexpr uint32_t kContextCtr = 0x10;
constexpr uint32_t kContextGpr = 0x18;
constexpr uint32_t kContextCr = 0x118;
constexpr uint32_t kContextXer = 0x11C;
// A doubleword with the FPSCR in its low half, then the 32 FPRs.
constexpr uint32_t kContextFpscr = 0x120;
constexpr uint32_t kContextFpr = 0x128;
// Whether the context is user mode's.
constexpr uint32_t kContextUserModeControl = 0x228;
// The VSCR as a vector register, then the 128 vector registers.
constexpr uint32_t kContextVscr = 0x230;
constexpr uint32_t kContextVr = 0x240;

// CONTEXT flags.
constexpr uint32_t kContextControl = 0x1;
constexpr uint32_t kContextFloatingPoint = 0x2;
constexpr uint32_t kContextInteger = 0x4;
constexpr uint32_t kContextVector = 0x10;

// A whole guest CONTEXT is this big, through the last vector register.
// 0xA40, which is also what the kernel's exception dispatch copies of one.
constexpr uint32_t kContextSize = kContextVr + 128 * 16;

// Writes the guest registers into a CONTEXT record, with the iar and lr given.
void StoreContextRecord(uint8_t* record, const cpu::ppc::PPCContext* context,
                        uint32_t iar, uint32_t lr);

// Loads the guest registers from a CONTEXT record, returning its iar.
uint32_t LoadContextRecord(const uint8_t* record,
                           cpu::ppc::PPCContext* context);

struct X_RTL_CRITICAL_SECTION;

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs,
                                    uint32_t cs_ptr);
X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs,
                                                    uint32_t cs_ptr,
                                                    uint32_t spin_count);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_RTL_H_
