/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_SYSTEM_MANIFEST_H_
#define XENIA_KERNEL_XAM_SYSTEM_MANIFEST_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace xe {
namespace kernel {
namespace xam {

// system.manifest lists the files a system update puts on the system extended
// partition. Each lives in a folder named for its version.
constexpr std::string_view kSystemExtPartition =
    "\\Device\\Harddisk0\\SystemExtPartition";
constexpr std::string_view kSystemManifestName = "system.manifest";

// The system version the manifest describes, or nothing when it isn't one.
std::optional<uint32_t> GetSystemManifestVersion(
    std::span<const uint8_t> manifest);

// The version of the file |name| the manifest lists.
std::optional<uint32_t> FindSystemManifestFile(
    std::span<const uint8_t> manifest, std::string_view name);

// The folder a file of |version| lives in.
std::string SystemExtFolderName(uint32_t version);

// The guest path of the system extended partition file |name|, or an empty
// string when no manifest lists it.
std::string FindSystemExtFile(std::string_view name);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_SYSTEM_MANIFEST_H_
