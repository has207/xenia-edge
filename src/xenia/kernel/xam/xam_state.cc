/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_state.h"
#include "xenia/emulator.h"
#include "xenia/vfs/virtual_file_system.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {
namespace xam {

XamState::XamState(Emulator* emulator, KernelState* kernel_state)
    : kernel_state_(kernel_state) {
  app_manager_ = std::make_unique<AppManager>();

  auto content_root = emulator->content_root();
  if (!content_root.empty()) {
    content_root = std::filesystem::absolute(content_root);
  }
  content_manager_ =
      std::make_unique<ContentManager>(kernel_state, content_root);

  user_tracker_ = std::make_unique<UserTracker>();
  profile_manager_ =
      std::make_unique<ProfileManager>(kernel_state, user_tracker_.get());
  achievement_manager_ = std::make_unique<AchievementManager>();

  LoadLanguageLocaleFallback();
  LoadLanguageTypefacePatch();
  LoadIptvServiceName();

  AppManager::RegisterApps(kernel_state, app_manager_.get());
}

void XamState::LoadLanguageLocaleFallback() {
  const std::array<std::u16string, 18> locale_data = {
      u"",      u"",      u"ja-JP", u"de-DE", u"fr-FR",  u"es-ES",
      u"it-IT", u"ko-KR", u"zh-TW", u"pt-BR", u"zh-CHS", u"pl-PL",
      u"ru-RU", u"sv-SE", u"tr-TR", u"nb-NO", u"nl-NL",  u"zh-CHS"};

  constexpr uint32_t array_start = 0x80D00000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(array_start, 0xC8, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    char16_t* ptr =
        kernel_state_->memory()->TranslateVirtual<char16_t*>(array_start);

    for (size_t i = 1; i < locale_data.size(); i++) {
      language_fallback_address_[i] =
          kernel_state_->memory()->HostToGuestVirtual(ptr);
      ptr += xe::string_util::copy_and_swap_truncating(
                 ptr, locale_data.at(i), locale_data.at(i).size() + 1) +
             1;
    }
  }
}

void XamState::LoadLanguageTypefacePatch() {
  const std::array<std::u16string, 7> patch_data = {
      u"",
      u"file://media:/XenonSCLatin.xttp2",
      u"file://media:/XenonCLatin.xttp2",
      u"file://media:/XenonJKLatin.xttp2",
      u"file://media:/XenonSCLatin.xttp1",
      u"file://media:/XenonCLatin.xttp1",
      u"file://media:/XenonJKLatin.xttp1"};

  constexpr uint32_t array_start = 0x80D10000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(array_start, 0x168, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    char16_t* ptr =
        kernel_state_->memory()->TranslateVirtual<char16_t*>(array_start);

    for (size_t i = 1; i < patch_data.size(); i++) {
      language_type_face_patch_[i] =
          kernel_state_->memory()->HostToGuestVirtual(ptr);
      ptr += xe::string_util::copy_and_swap_truncating(
                 ptr, patch_data.at(i), patch_data.at(i).size() + 1) +
             1;
    }
  }
}

uint32_t XamState::GetLanguageTypefacePatch(uint32_t language) const {
  // The console's xam returns the xttp1 patch when the boot loader flags in
  // XboxHardwareInfo have 0x1000, the xttp2 one with 0x2000, and none
  // otherwise. Those flags tell which patch its flash has, so take the patch
  // the system files have here.
  uint32_t index = 0;
  if (language == 17) {  // kSChinese
    index = 1;
  } else if (language == 8) {  // kTChinese
    index = 2;
  } else {
    index = 3;
  }
  static constexpr std::string_view kNames[] = {"XenonSCLatin", "XenonCLatin",
                                                "XenonJKLatin"};
  const std::string_view name = kNames[index - 1];
  auto file_system = kernel_state_->file_system();
  if (file_system->ResolvePath(fmt::format("media:\\{}.xttp1", name))) {
    return language_type_face_patch_[index + 3];
  }
  if (file_system->ResolvePath(fmt::format("media:\\{}.xttp2", name))) {
    return language_type_face_patch_[index];
  }
  return 0;
}

void XamState::LoadIptvServiceName() {
  constexpr uint32_t address = 0x80D20000;

  if (kernel_state_->memory()
          ->LookupHeap(0x80000000)
          ->AllocFixed(address, 0x78, 0x1000, kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite)) {
    iptv_name_address_ = address;
  }
}

UserProfile* XamState::GetUserProfile(uint32_t user_index) const {
  if (user_index >= XUserMaxUserCount && user_index < XUserIndexLatest) {
    return nullptr;
  }

  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index));
}

UserProfile* XamState::GetUserProfile(uint64_t xuid) const {
  return profile_manager_->GetProfile(xuid);
}

UserProfile* XamState::GetUserProfileLive(uint64_t xuid) const {
  return profile_manager_->GetProfileLive(xuid);
}

bool XamState::IsUserSignedIn(uint32_t user_index) const {
  return profile_manager_->GetProfile(static_cast<uint8_t>(user_index)) !=
         nullptr;
}

bool XamState::IsUserSignedIn(uint64_t xuid) const {
  return GetUserProfile(xuid) != nullptr;
}

void XamState::LoadSpaInfo(const SpaInfo* info) {
  if (!info) {
    return;
  }
  // Check if we have loaded SpaInfo already. If yes then check currently loaded
  // version.
  if (spa_info_) {
    // Trying to load spa with lower version, for whatever reason.
    if (*info <= *spa_info_) {
      return;
    }
  }

  spa_info_ = std::make_unique<SpaInfo>(*info);
  spa_info_->Load();
  user_tracker_->UpdateSpaInfo(spa_info_.get());
}

void XamState::SetContentRegisterCallback(uint32_t callback) {
  content_register_callback = callback;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
