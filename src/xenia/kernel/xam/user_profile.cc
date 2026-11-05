/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/user_profile.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/png_utils.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xdbf/gpd_info.h"

namespace xe {
namespace kernel {
namespace xam {

UserProfile::UserProfile(const uint64_t xuid,
                         const X_XAMACCOUNTINFO* account_info)
    : xuid_(xuid), account_info_(*account_info), profile_images_() {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."
  LoadProfileGpds();

  // Load default gamer tiles
  LoadProfileIcon(XTileType::kGamerTile);
  LoadProfileIcon(XTileType::kGamerTileSmall);

  // Also load personal gamer tiles (custom profile pictures)
  // Note: These use the same filenames as regular tiles, so if a personal
  // tile exists, it will overwrite the default tile file. We load both types
  // to maintain compatibility - the personal tile will be preferred when
  // present.
  LoadProfileIcon(XTileType::kPersonalGamerTile);
  LoadProfileIcon(XTileType::kPersonalGamerTileSmall);

  // A picture on disk in one size gets the other here, since titles can ask
  // for either.
  AddMissingIconSize(XTileType::kGamerTile, XTileType::kGamerTileSmall);
  AddMissingIconSize(XTileType::kPersonalGamerTile,
                     XTileType::kPersonalGamerTileSmall);
}

GpdInfo* UserProfile::GetGpd(const uint32_t title_id) {
  return const_cast<GpdInfo*>(
      const_cast<const UserProfile*>(this)->GetGpd(title_id));
}

const GpdInfo* UserProfile::GetGpd(const uint32_t title_id) const {
  if (title_id == kDashboardID) {
    return &dashboard_gpd_;
  }

  if (!games_gpd_.count(title_id)) {
    return nullptr;
  }

  return &games_gpd_.at(title_id);
}

void UserProfile::LoadProfileGpds() {
  // First load dashboard GPD because it stores all opened games
  dashboard_gpd_ = LoadGpd(kDashboardID);
  if (!dashboard_gpd_.IsValid()) {
    dashboard_gpd_ = GpdInfoProfile();
  }

  const auto gpds_to_load = dashboard_gpd_.GetTitlesInfo();

  for (const auto& gpd : gpds_to_load) {
    const auto gpd_data = LoadGpd(gpd->title_id);
    if (gpd_data.empty()) {
      continue;
    }

    // Use insert_or_assign to replace existing GPDs when reloading
    games_gpd_.insert_or_assign(gpd->title_id,
                                GpdInfoTitle(gpd->title_id, gpd_data));
  }
}

void UserProfile::LoadProfileIcon(XTileType tile_type) {
  if (!kTileFileNames.count(tile_type)) {
    return;
  }

  const std::string path =
      fmt::format("User_{:016X}:\\{}", xuid_, kTileFileNames.at(tile_type));

  vfs::File* file = nullptr;
  vfs::FileAction action;

  const X_STATUS result = kernel_state()->file_system()->OpenFile(
      nullptr, path, vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead,
      false, true, &file, &action);

  if (result != X_STATUS_SUCCESS) {
    return;
  }

  std::vector<uint8_t> data(file->entry()->size());
  size_t written_bytes = 0;
  file->ReadSync(std::span<uint8_t>(data.data(), file->entry()->size()), 0,
                 &written_bytes);
  file->Destroy();

  profile_images_.insert_or_assign(tile_type, data);
}

void UserProfile::AddMissingIconSize(XTileType large_type,
                                     XTileType small_type) {
  const bool has_large = profile_images_.contains(large_type);
  if (has_large == profile_images_.contains(small_type)) {
    return;
  }
  const auto [width, height] =
      has_large ? kProfileIconSizeSmall : kProfileIconSize;
  auto resized = ResizePng(
      profile_images_.at(has_large ? large_type : small_type), width, height);
  if (!resized.empty()) {
    profile_images_.insert_or_assign(has_large ? small_type : large_type,
                                     std::move(resized));
  }
}

void UserProfile::ClearProfileIcon(XTileType icon_type) {
  profile_images_.erase(icon_type);
  if (kTileFileNames.contains(icon_type)) {
    kernel_state()->file_system()->DeletePath(
        fmt::format("User_{:016X}:\\{}", xuid_, kTileFileNames.at(icon_type)));
  }
}

void UserProfile::WriteProfileIcon(XTileType tile_type,
                                   std::span<const uint8_t> icon_data) {
  const std::string path =
      fmt::format("User_{:016X}:\\{}", xuid_, kTileFileNames.at(tile_type));

  vfs::File* file = nullptr;
  vfs::FileAction action;

  const X_STATUS result = kernel_state()->file_system()->OpenFile(
      nullptr, path, vfs::FileDisposition::kOverwriteIf,
      vfs::FileAccess::kGenericWrite | vfs::FileAccess::kFileWriteData, false,
      true, &file, &action);

  if (result != X_STATUS_SUCCESS) {
    return;
  }

  // Set the file length first to ensure we can write
  X_STATUS set_length_result = file->SetLength(icon_data.size());
  if (set_length_result != X_STATUS_SUCCESS &&
      set_length_result != X_STATUS_NOT_IMPLEMENTED) {
    XELOGW("WriteProfileIcon: SetLength failed with status {:08X}",
           set_length_result);
  }

  size_t written_bytes = 0;

  X_STATUS write_result =
      file->WriteSync({icon_data.data(), icon_data.size()}, 0, &written_bytes);
  if (write_result != X_STATUS_SUCCESS) {
    XELOGW("WriteProfileIcon: WriteSync failed with status {:08X}",
           write_result);
  } else {
    XELOGI("WriteProfileIcon: Successfully wrote {} bytes to {}", written_bytes,
           path);
  }

  file->Destroy();

  profile_images_.insert_or_assign(
      tile_type, std::vector<uint8_t>(icon_data.begin(), icon_data.end()));
}

std::vector<uint8_t> UserProfile::LoadGpd(const uint32_t title_id) {
  auto entry = kernel_state()->file_system()->ResolvePath(
      fmt::format("User_{:016X}:\\{:08X}.gpd", xuid_, title_id));

  if (!entry) {
    XELOGW("User {} (XUID: {:016X}) doesn't have profile GPD!", name(), xuid());
    return {};
  }

  vfs::File* file;
  auto result = entry->Open(vfs::FileAccess::kFileReadData, &file);
  if (result != X_STATUS_SUCCESS) {
    XELOGW("User {} (XUID: {:016X}) cannot open profile GPD!", name(), xuid());
    return {};
  }

  std::vector<uint8_t> data(entry->size());

  size_t read_size = 0;
  result = file->ReadSync(std::span<uint8_t>(data.data(), entry->size()), 0,
                          &read_size);
  if (result != X_STATUS_SUCCESS || read_size != entry->size()) {
    XELOGW(
        "User {} (XUID: {:016X}) cannot read profile GPD! Status: {:08X} read: "
        "{}/{} bytes",
        name(), xuid(), result, read_size, entry->size());
    return {};
  }

  file->Destroy();
  return data;
}

bool UserProfile::WriteGpd(const uint32_t title_id) {
  const GpdInfo* gpd = GetGpd(title_id);
  if (!gpd) {
    return false;
  }

  std::vector<uint8_t> data = gpd->Serialize();

  vfs::File* file = nullptr;
  vfs::FileAction action;

  const std::string mounted_path =
      fmt::format("User_{:016X}:\\{:08X}.gpd", xuid_, title_id);

  const X_STATUS result = kernel_state()->file_system()->OpenFile(
      nullptr, mounted_path, vfs::FileDisposition::kOverwriteIf,
      vfs::FileAccess::kGenericWrite, false, true, &file, &action);

  if (result != X_STATUS_SUCCESS) {
    return false;
  }

  size_t written_bytes = 0;
  file->WriteSync(std::span<uint8_t>(data.data(), data.size()), 0,
                  &written_bytes);
  file->Destroy();
  return true;
}

bool UserProfile::RemoveGpd(const uint32_t title_id) {
  auto it = games_gpd_.find(title_id);
  if (it == games_gpd_.end()) {
    return false;
  }

  const std::string mounted_path =
      fmt::format("User_{:016X}:\\{:08X}.gpd", xuid_, title_id);

  if (!kernel_state()->file_system()->DeletePath(mounted_path)) {
    return false;
  }

  games_gpd_.erase(it);
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
