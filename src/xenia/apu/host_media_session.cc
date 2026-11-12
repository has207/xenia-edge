/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/host_media_session.h"

#include "xenia/base/platform.h"

namespace xe {
namespace apu {

#if !XE_PLATFORM_WIN32
std::unique_ptr<HostMediaSession> HostMediaSession::Create() { return nullptr; }
#endif  // !XE_PLATFORM_WIN32

}  // namespace apu
}  // namespace xe
