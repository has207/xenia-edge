/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/indie_game.h"

#include <charconv>
#include <cstring>
#include <functional>
#include <memory>

#include "third_party/pugixml/src/pugixml.hpp"
#include "xenia/base/filesystem.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// The record the dashboard passes the launcher: a magic, the package's file
// name and the device the package is on.
constexpr uint32_t kLaunchDataMagic = 0xCAFEBABE;
constexpr size_t kLaunchDataFileNameOffset = 4;
constexpr size_t kLaunchDataFileNameSize = 0x2A;
constexpr size_t kLaunchDataDeviceOffset = 0x30;
constexpr size_t kLaunchDataSize = 0x34;

static std::vector<uint8_t> ReadDeviceFile(vfs::Device& device,
                                           std::string_view name) {
  vfs::Entry* entry = device.ResolvePath(name);
  vfs::File* file = nullptr;
  if (!entry || XFAILED(entry->Open(vfs::FileAccess::kFileReadData, &file))) {
    return {};
  }
  std::vector<uint8_t> data(entry->size());
  size_t bytes_read = 0;
  const X_STATUS status = file->ReadSync(data, 0, &bytes_read);
  file->Destroy();
  if (XFAILED(status) || bytes_read != data.size()) {
    return {};
  }
  return data;
}

std::optional<IndieGame> ReadIndieGame(
    const std::filesystem::path& package_path) {
  // Reads a file in the package's root, empty when there's none.
  std::function<std::vector<uint8_t>(std::string_view)> read;
  std::unique_ptr<vfs::XContentContainerDevice> device;
  std::error_code ec;
  if (std::filesystem::is_directory(package_path, ec)) {
    read = [&package_path](std::string_view name) {
      const auto path =
          xe::filesystem::FindChildIgnoringCase(package_path, name);
      return path.empty() ? std::vector<uint8_t>()
                          : xe::filesystem::ReadAllBytes(path);
    };
  } else {
    // A package can also be installed as the package file itself.
    const auto header =
        vfs::XContentContainerDevice::ReadContainerHeader(package_path);
    if (!header || !header->content_header.is_magic_valid() ||
        header->content_metadata.content_type.get() !=
            XContentType::kMarketplaceContent ||
        header->content_metadata.execution_info.title_id.get() != kXN_2002) {
      return std::nullopt;
    }
    device =
        vfs::XContentContainerDevice::CreateContentDevice("", package_path);
    if (!device || !device->Initialize()) {
      return std::nullopt;
    }
    read = [&device](std::string_view name) {
      return ReadDeviceFile(*device, name);
    };
  }

  const auto xml = read("GameInfo.xml");
  pugi::xml_document doc;
  if (xml.empty() || !doc.load_buffer(xml.data(), xml.size())) {
    return std::nullopt;
  }
  const pugi::xml_node info = doc.child("GameInfo").child("TitleInfo");
  // In decimal, as is the launcher's own ID beside it.
  const std::string_view id = info.attribute("TitleID").value();
  uint32_t title_id = 0;
  const auto [end, error] =
      std::from_chars(id.data(), id.data() + id.size(), title_id);
  if (error != std::errc() || end != id.data() + id.size() || !title_id) {
    return std::nullopt;
  }
  IndieGame game;
  game.title_id = title_id;
  game.name = info.attribute("Name").value();
  game.virtual_title_id = info.attribute("VirtualTitleID").value();
  game.package_path = package_path;
  const std::string_view image = info.attribute("ImagePath").value();
  if (!image.empty()) {
    game.icon = read(image);
  }
  return game;
}

std::vector<uint8_t> MakeIndieGameLaunchData(std::string_view file_name,
                                             uint32_t device_id) {
  if (file_name.empty() || file_name.size() > kLaunchDataFileNameSize) {
    return {};
  }
  std::vector<uint8_t> data(kLaunchDataSize);
  xe::store_and_swap<uint32_t>(data.data(), kLaunchDataMagic);
  std::memcpy(data.data() + kLaunchDataFileNameOffset, file_name.data(),
              file_name.size());
  xe::store_and_swap<uint32_t>(data.data() + kLaunchDataDeviceOffset,
                               device_id);
  return data;
}

std::string IndieGameLaunchFileName(std::span<const uint8_t> launch_data) {
  if (launch_data.size() < kLaunchDataSize ||
      xe::load_and_swap<uint32_t>(launch_data.data()) != kLaunchDataMagic) {
    return {};
  }
  const std::string_view field(
      reinterpret_cast<const char*>(launch_data.data()) +
          kLaunchDataFileNameOffset,
      kLaunchDataFileNameSize);
  return std::string(field.substr(0, field.find('\0')));
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
