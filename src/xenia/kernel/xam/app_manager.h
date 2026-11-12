/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APP_MANAGER_H_
#define XENIA_KERNEL_XAM_APP_MANAGER_H_

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

class App {
 public:
  // What a request leaves in its XOVERLAPPED.
  struct Completion {
    uint32_t result;
    uint32_t extended_error;
    uint32_t length;
  };

  uint32_t app_id() const { return app_id_; }

  virtual X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                        uint32_t buffer_length) = 0;
  // Each app completes its own requests.
  virtual Completion CompleteRequest(X_HRESULT result) const;

  virtual ~App() = default;

 protected:
  App(KernelState* kernel_state, uint32_t app_id);

  KernelState* kernel_state_;
  Memory* memory_;
  uint32_t app_id_;
};

class AppManager {
 public:
  static void RegisterApps(KernelState* kernel_state, AppManager* manager);

  void RegisterApp(std::unique_ptr<App> app);

  X_HRESULT DispatchMessageSync(uint32_t app_id, uint32_t message,
                                uint32_t buffer_ptr, uint32_t buffer_length);
  // With an overlapped the message runs later on a copy of the buffer and this
  // returns X_ERROR_IO_PENDING.
  X_HRESULT DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                 uint32_t buffer_ptr, uint32_t buffer_length,
                                 uint32_t overlapped_ptr = 0);

 private:
  std::vector<std::unique_ptr<App>> apps_;
  std::unordered_map<uint32_t, App*> app_lookup_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APP_MANAGER_H_
