/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/app_manager.h"

#include <algorithm>
#include <cstring>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/apps/messenger_app.h"
#include "xenia/kernel/xam/apps/xam_app.h"
#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xam/apps/xlivebase_app.h"
#include "xenia/kernel/xam/apps/xmp_app.h"

namespace xe {
namespace kernel {
namespace xam {

App::App(KernelState* kernel_state, uint32_t app_id)
    : kernel_state_(kernel_state),
      memory_(kernel_state->memory()),
      app_id_(app_id) {}

App::Completion App::CompleteRequest(X_HRESULT result) const {
  // Some titles take a nonzero length as failure.
  return {result, result, result ? 0xFFFFFFFF : 0};
}

void AppManager::RegisterApps(KernelState* kernel_state, AppManager* manager) {
  manager->RegisterApp(std::make_unique<apps::MessengerApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XmpApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XgiApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XLiveBaseApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XamApp>(kernel_state));
}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  assert_zero(app_lookup_.count(app->app_id()));
  app_lookup_.insert({app->app_id(), app.get()});
  apps_.push_back(std::move(app));
}

X_HRESULT AppManager::DispatchMessageSync(uint32_t app_id, uint32_t message,
                                          uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }
  return it->second->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

X_HRESULT AppManager::DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length,
                                           uint32_t overlapped_ptr) {
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }
  App* app = it->second;
  if (!overlapped_ptr) {
    return app->DispatchMessageSync(message, buffer_ptr, buffer_length);
  }

  // A title can depend on the request staying pending for a while after this
  // returns: one registers its in-game message handler only in a frame that
  // sees the session it hosts still being created. The delay before deferred
  // work runs provides that. The caller's buffer may be gone by then.
  // TODO(has207): Give XMsg requests their own worker. The kernel dispatch
  // thread also runs DPCs and XAM dialogs, which hold it until they close.
  Memory* memory = kernel_memory();
  uint32_t copy_ptr = 0;
  if (buffer_ptr) {
    // A zero-length request still gets a valid pointer.
    copy_ptr = memory->SystemHeapAlloc(std::max(buffer_length, 1u));
    if (!copy_ptr) {
      return X_E_FAIL;
    }
    std::memcpy(memory->TranslateVirtual(copy_ptr),
                memory->TranslateVirtual(buffer_ptr), buffer_length);
  }
  kernel_state()->CompleteOverlappedDeferredEx(
      [app, message, copy_ptr, buffer_length](uint32_t& extended_error,
                                              uint32_t& length) -> X_RESULT {
        const App::Completion completion = app->CompleteRequest(
            app->DispatchMessageSync(message, copy_ptr, buffer_length));
        extended_error = completion.extended_error;
        length = completion.length;
        return completion.result;
      },
      overlapped_ptr, nullptr,
      [memory, copy_ptr]() {
        if (copy_ptr) {
          memory->SystemHeapFree(copy_ptr);
        }
      });
  return X_ERROR_IO_PENDING;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
