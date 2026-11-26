/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsymboliclink.h"

#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

XSymbolicLink::XSymbolicLink(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType), path_(), target_() {}

XSymbolicLink::~XSymbolicLink() {}

void XSymbolicLink::Initialize(const std::string_view path,
                               const std::string_view target) {
  path_ = std::string(path);
  target_ = std::string(target);
  // TODO(gibbed): kernel_state_->RegisterSymbolicLink(this);
}

}  // namespace kernel
}  // namespace xe
