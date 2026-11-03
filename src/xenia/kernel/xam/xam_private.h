/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_PRIVATE_H_
#define XENIA_KERNEL_XAM_XAM_PRIVATE_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/xam/xam_ordinals.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class XModule;
namespace xam {

xe::cpu::Export* RegisterExport_xam(xe::cpu::Export* export_entry);

// XamLoaderLaunchTitleEx, which doesn't return.
void LaunchTitle(std::string path, std::string_view d_drive_path,
                 uint32_t flags);
// Launches the title at |host_path| on the host, from its default module.
// Doesn't return.
void LaunchHostTitle(const std::filesystem::path& host_path, uint32_t flags);

// Loads the XUI string table |file_name| names in |module|'s |section|
// package, or the section itself without a name, as XuiLoadStringTableFromFile
// does.
X_HRESULT LoadXuiStringTable(XModule& module, std::string_view section,
                             std::string_view file_name, uint32_t* table_ptr);
// The string at |index| in a table LoadXuiStringTable loaded, or 0.
uint32_t LookupXuiStringTable(uint32_t table, uint32_t index);

// Registration functions, one per file.
#define XE_MODULE_EXPORT_GROUP(m, n)                                  \
  void Register##n##Exports(xe::cpu::ExportResolver* export_resolver, \
                            KernelState* kernel_state);
#include "xam_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_PRIVATE_H_
