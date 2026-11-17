/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_module.h"

#include <fstream>
#include <iomanip>
#include <sstream>

#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_system_link.h"

namespace xe {
namespace kernel {
namespace xam {

XamModule::XamModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xam.xex"),
      loader_data_(),
      ethernet_intercept_(kernel_state) {
  RegisterExportTable(export_resolver_);

  // Register all exported functions.
#define XE_MODULE_EXPORT_GROUP(m, n) \
  Register##n##Exports(export_resolver_, kernel_state_);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP
}

static auto& get_xam_exports() {
  static std::vector<xe::cpu::Export*> xam_exports(4096);
  return xam_exports;
}

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry) {
  auto& xam_exports = get_xam_exports();
  assert_true(export_entry->ordinal < xam_exports.size());
  xam_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}
// Build the export table used for resolution.
#include "xenia/kernel/util/export_table_pre.inc"
static constexpr xe::cpu::Export xam_export_table[] = {
#include "xenia/kernel/xam/xam_table.inc"
};
#include "xenia/kernel/util/export_table_post.inc"
void XamModule::RegisterExportTable(xe::cpu::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);
  auto& xam_exports = get_xam_exports();

  for (size_t i = 0; i < xe::countof(xam_export_table); ++i) {
    auto& export_entry = xam_export_table[i];
    assert_true(export_entry.ordinal < xam_exports.size());
    if (!xam_exports[export_entry.ordinal]) {
      xam_exports[export_entry.ordinal] =
          const_cast<xe::cpu::Export*>(&export_entry);
    }
  }
  export_resolver->RegisterTable("xam.xex", &get_xam_exports());
}

XamModule::~XamModule() {}

std::shared_ptr<net::SystemLink> XamModule::system_link() {
  std::lock_guard lock(system_link_mutex_);
  if (!system_link_started_) {
    system_link_started_ = true;
    system_link_ = CreateSystemLink(kernel_state_, system_link_port_);
  }
  return system_link_;
}

void XamModule::ShutdownSystemLink() {
  // Destroyed once the mutex is released, as that joins the worker.
  std::shared_ptr<net::SystemLink> link;
  {
    std::lock_guard lock(system_link_mutex_);
    system_link_started_ = true;
    link = std::move(system_link_);
  }
}

uint32_t XamModule::SetSystemLinkPort(uint16_t port) {
  std::lock_guard lock(system_link_mutex_);
  if (system_link_) {
    return system_link_->SetSystemLinkPort(port);
  }
  system_link_port_ = port;
  return 0;
}

uint16_t XamModule::system_link_port() {
  std::lock_guard lock(system_link_mutex_);
  return system_link_ ? system_link_->system_link_port() : system_link_port_;
}

void XamModule::StartXNet(bool wsa, std::span<const uint8_t> params) {
  std::lock_guard lock(xnet_mutex_);
  if (!xnet_starts_ && !wsa_starts_) {
    xnet_startup_params_ = net::NormalizeStartupParams(params);
  }
  ++(wsa ? wsa_starts_ : xnet_starts_);
}

void XamModule::CleanupXNet(bool wsa) {
  std::lock_guard lock(xnet_mutex_);
  auto& starts = wsa ? wsa_starts_ : xnet_starts_;
  if (starts) {
    --starts;
  }
}

net::StartupParams XamModule::xnet_startup_params() {
  std::lock_guard lock(xnet_mutex_);
  return xnet_startup_params_;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
