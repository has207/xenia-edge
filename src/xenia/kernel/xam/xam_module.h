/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_MODULE_H_
#define XENIA_KERNEL_XAM_XAM_MODULE_H_

#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/kernel/xam/xam_xhttp.h"
#include "xenia/net/system_link.h"

namespace xe {
namespace kernel {
namespace xam {

class XamModule : public KernelModule {
 public:
  XamModule(Emulator* emulator, KernelState* kernel_state);
  virtual ~XamModule();

  static void RegisterExportTable(xe::cpu::ExportResolver* export_resolver);

  struct LoaderData {
    bool launch_data_present = false;
    std::string host_path;  // Full host path to next title to load
    std::string
        launch_path;  // Full guest path to next xex. might be default.xex

    uint32_t launch_flags = 0;
    std::vector<uint8_t> launch_data;
  };

  void SetLaunchCallback(uint32_t callback) { callback_ = callback; }

  const LoaderData& loader_data() const { return loader_data_; }
  LoaderData& loader_data() { return loader_data_; }

  // Started on first use, see GetSystemLink.
  std::shared_ptr<net::SystemLink> system_link();
  // Moves System Link to the resolved UDP |port|, or picks the port it starts
  // on when it hasn't started yet.
  uint32_t SetSystemLinkPort(uint16_t port);
  uint16_t system_link_port();
  EthernetIntercept& ethernet_intercept() { return ethernet_intercept_; }

  // XNetStartup, or WSAStartup with |wsa| and no |params|. Only the first
  // start since everything was cleaned up takes its parameters.
  void StartXNet(bool wsa, std::span<const uint8_t> params);
  // XNetCleanup, or WSACleanup with |wsa|.
  void CleanupXNet(bool wsa);
  net::StartupParams xnet_startup_params();

  const std::shared_ptr<XHttpState>& xhttp_state() const {
    return xhttp_state_;
  }

 private:
  uint32_t callback_ = 0;
  LoaderData loader_data_;

  std::mutex system_link_mutex_;
  bool system_link_started_ = false;
  std::shared_ptr<net::SystemLink> system_link_;
  uint16_t system_link_port_ = net::secure::kSystemLinkPort;
  EthernetIntercept ethernet_intercept_;

  std::mutex xnet_mutex_;
  uint32_t xnet_starts_ = 0, wsa_starts_ = 0;
  net::StartupParams xnet_startup_params_ = net::NormalizeStartupParams({});

  std::shared_ptr<XHttpState> xhttp_state_ = CreateXHttpState();
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_MODULE_H_
