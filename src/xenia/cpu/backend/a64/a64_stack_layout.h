/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_
#define XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_

#include "xenia/base/vec128.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class StackLayout {
 public:
  /**
   * ARM64 Thunk Stack Layout (HostToGuest)
   * NOTE: stack must always be 16-byte aligned.
   *
   *  +------------------+
   *  | x19, x20         | sp + 0x000
   *  | x21, x22         | sp + 0x010
   *  | x23, x24         | sp + 0x020
   *  | x25, x26         | sp + 0x030
   *  | x27, x28         | sp + 0x040
   *  | x29 (fp), x30    | sp + 0x050
   *  | q8, q9           | sp + 0x060  (full 128-bit, used by JIT)
   *  | q10, q11         | sp + 0x080
   *  | q12, q13         | sp + 0x0A0
   *  | q14, q15         | sp + 0x0C0
   *  +------------------+
   *  Total: 0xE0 = 224 bytes (16-byte aligned)
   */
  static constexpr size_t THUNK_STACK_SIZE = 224;

  /**
   * ARM64 Guest Stack Layout
   *  +------------------+
   *  | scratch, 48b     | sp + 0x000  (3 x Q for VMX FP scratch)
   *  | guest ret addr   | sp + 0x030  (guest PPC return address)
   *  | host ret addr    | sp + 0x038  (host x30/LR, for ret instruction)
   *  | call ret addr    | sp + 0x040  (next call's guest PPC return addr)
   *  | stackpoint prev  | sp + 0x048  (A64StackpointNode::prev_)
   *  | guest r1, lr     | sp + 0x050  (A64StackpointNode guest words)
   *  | padding          | sp + 0x058
   *  |  ... locals ...  | sp + 0x060
   *  +------------------+
   *
   * Minimum size: 96 bytes (aligned to 16).
   *
   * Convention: at guest function entry, x0 holds the guest PPC return
   * address. The prolog stores it to GUEST_RET_ADDR and saves x30 (host
   * LR) to HOST_RET_ADDR.
   */
  static constexpr size_t GUEST_STACK_SIZE = 96;  // 16-byte aligned
  static constexpr size_t GUEST_SCRATCH = 0;      // 48 bytes (3 x Q)
  // GUEST_RET_ADDR and HOST_RET_ADDR are adjacent on purpose: the prolog
  // saves both with one stp and the call-return teardown reloads both with
  // one ldp.
  static constexpr size_t GUEST_RET_ADDR = 48;
  static constexpr size_t HOST_RET_ADDR = 56;
  static constexpr size_t GUEST_CALL_RET_ADDR = 64;
  // This frame's stackpoint node (A64StackpointNode), linked through
  // stackpoint_head in A64BackendContext. The chain has no depth counter or
  // bound; the node address minus STACKPOINT_PREV is the frame's post-alloc
  // SP, which is what the longjmp repair restores. (The pending-repair marker
  // itself stays in A64BackendContext so it can be checked even when native
  // SP still points at a skipped frame.)
  static constexpr size_t STACKPOINT_PREV = 72;
  static constexpr size_t STACKPOINT_GUEST_SP = 80;
  static constexpr size_t STACKPOINT_GUEST_RET = 84;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_STACK_LAYOUT_H_
