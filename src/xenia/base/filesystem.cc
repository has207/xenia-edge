/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/filesystem.h"

#include <algorithm>
#include <fstream>
#include <ios>
#include <utility>

#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"

namespace xe {
namespace filesystem {

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return {};
  }
  const std::streamoff size = f.tellg();
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(data.data()), data.size())) {
    return {};
  }
  return data;
}

bool WriteAllBytes(const std::filesystem::path& path,
                   std::span<const uint8_t> data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(data.data()),
          std::streamsize(data.size()));
  f.close();
  return !f.fail();
}

std::filesystem::path FindChildIgnoringCase(const std::filesystem::path& parent,
                                            const std::string_view name) {
  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(parent, ec)) {
    if (xe::utf8::equal_case(xe::path_to_utf8(child.path().filename()), name)) {
      return child.path();
    }
  }
  return {};
}

std::string ReadAllText(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return {};
  }
  const std::streamoff size = f.tellg();
  if (size <= 0) {
    return {};
  }
  std::string data(static_cast<size_t>(size), '\0');
  f.seekg(0);
  if (!f.read(data.data(), data.size())) {
    return {};
  }
  return data;
}

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

std::error_code CreateFolder(const std::filesystem::path& path) {
  if (std::filesystem::exists(path)) {
    return {};
  }

  std::error_code ec;
  if (std::filesystem::create_directories(path, ec)) {
    return {};
  }

  return ec;
}

// Host listing order is not portable, ntfs collates while ext4 hands back
// hash order, so a directory would enumerate differently per platform. Collate
// on the uppercased name to match ntfs, keeping windows behavior untouched.
// Not console accurate, an stfs package enumerates in slot order, i.e. creation
// order, which host filesystems cannot reproduce since creation time does not
// survive copies on windows and moves on write on posix.
static bool CollatesBefore(const FileInfo& left, const FileInfo& right) {
  return xe::utf8::upper_ascii(xe::path_to_utf8(left.name)) <
         xe::utf8::upper_ascii(xe::path_to_utf8(right.name));
}

std::vector<FileInfo> ListFiles(const std::filesystem::path& path) {
  std::vector<FileInfo> files = internal::ListFilesUnsorted(path);
  std::ranges::sort(files, CollatesBefore);
  return files;
}

std::vector<FileInfo> ListDirectories(const std::filesystem::path& path) {
  std::vector<FileInfo> files = ListFiles(path);
  std::vector<FileInfo> directories = {};

  std::ranges::copy_if(std::as_const(files), std::back_inserter(directories),
                       [](const FileInfo& file) {
                         return file.type == FileInfo::Type::kDirectory;
                       });

  return directories;
}

std::vector<FileInfo> FilterByName(const std::vector<FileInfo>& files,
                                   const std::regex pattern) {
  std::vector<FileInfo> filtered_entries = {};

  std::ranges::copy_if(files, std::back_inserter(filtered_entries),
                       [pattern](const FileInfo& file) {
                         return std::regex_match(
                             xe::path_to_utf8(file.name.filename()), pattern);
                       });
  return filtered_entries;
}

std::vector<FileInfo> FindFileWithName(const std::filesystem::path& path,
                                       std::string_view name, bool recursive) {
  if (!std::filesystem::exists(path)) {
    return {};
  }

  if (!std::filesystem::is_directory(path)) {
    return {};
  }

  if (!recursive) {
    return FilterByName(ListFiles(path), std::regex(std::string(name)));
  }

  const std::string file_name = xe::utf8::lower_ascii(name);

  std::vector<FileInfo> filtered_entries = {};
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(path)) {
    if (entry.is_regular_file() && xe::utf8::lower_ascii(xe::path_to_utf8(
                                       entry.path().filename())) == file_name) {
      auto file_info = GetInfo(entry.path());
      if (file_info) {
        filtered_entries.push_back(std::move(file_info.value()));
      }
    }
  }
  return filtered_entries;
}

}  // namespace filesystem
}  // namespace xe
