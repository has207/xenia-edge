/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_XHTTP_H_
#define XENIA_KERNEL_XAM_XAM_XHTTP_H_

#include <memory>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"

namespace xe {
namespace kernel {
namespace xam {

#pragma pack(push, 4)
struct X_PERFORMANCE_COUNTERS {
  // Handles open of each type.
  xe::be<uint32_t> requests;
  xe::be<uint32_t> connections;
  xe::be<uint32_t> sessions;
  xe::be<uint64_t> bytes_received;
  xe::be<uint64_t> bytes_sent;
};
static_assert_size(X_PERFORMANCE_COUNTERS, 0x1C);
#pragma pack(pop)

// The XHttp handles a title has open. XamModule holds it so it goes with the
// title's kernel and nothing from an earlier run is left over on relaunch.
struct XHttpState;
std::shared_ptr<XHttpState> CreateXHttpState();

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_XHTTP_H_
