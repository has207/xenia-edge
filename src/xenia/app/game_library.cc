/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_library.h"

#include <algorithm>
#include <fstream>
#include <regex>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"

namespace xe {
namespace app {

// A release folder name, or empty when the name is not one. Only the exact
// spelling VersionToString() produces is accepted, so one version can never
// end up naming two folders.
static std::optional<uint32_t> ParseVersionFolder(
    const std::filesystem::path& name) {
  const auto parsed = vfs::XexVersion::FromString(xe::path_to_utf8(name));
  return parsed ? std::optional<uint32_t>(parsed->value()) : std::nullopt;
}

// '/' separators avoid TOML escaping and round-trip through std::filesystem.
static std::string PathToToml(const std::filesystem::path& path) {
  std::string s = xe::path_to_utf8(path);
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

// Force exactly one default: keep the first flagged one, else flag the first.
static void NormalizeDefault(LibraryEntry& entry) {
  bool seen = false;
  for (auto& p : entry.paths) {
    if (p.is_default && !seen) {
      seen = true;
    } else {
      p.is_default = false;
    }
  }
  if (!seen && !entry.paths.empty()) {
    entry.paths.front().is_default = true;
  }
}

const LibraryPath& LibraryEntry::default_path() const {
  assert_false(paths.empty());
  for (const auto& p : paths) {
    if (p.is_default) {
      return p;
    }
  }
  return paths.front();
}

std::string VersionToString(uint32_t version) {
  return vfs::XexVersion::FromValue(version).ToString();
}

std::optional<GameFileInfo> ReadGameFileInfo(
    const std::filesystem::path& path) {
  const auto extension =
      xe::utf8::lower_ascii(xe::path_to_utf8(path.extension()));
  if (extension == ".xex") {
    if (auto m = vfs::ExtractXexMetadata(path)) {
      return GameFileInfo{m->media_id, m->version.value(), m->disc_number,
                          m->disc_count};
    }
  } else if (extension == ".iso") {
    if (auto m = vfs::ExtractIsoMetadata(path)) {
      return GameFileInfo{m->media_id, m->version.value(), m->disc_number,
                          m->disc_count};
    }
  } else if (extension == ".zar") {
    if (auto m = vfs::ExtractZarMetadata(path)) {
      return GameFileInfo{m->media_id, m->version.value(), m->disc_number,
                          m->disc_count};
    }
  } else if (auto m = vfs::ExtractStfsMetadata(path)) {
    // The container describes this release specifically, so take all of it
    // while we are here. The title-level record cannot tell releases apart.
    return GameFileInfo{m->media_id,
                        m->version,
                        m->disc_number,
                        m->disc_count,
                        std::move(m->icon_data),
                        std::move(m->title_name),
                        std::move(m->display_name)};
  }
  return std::nullopt;
}

GameLibrary::GameLibrary(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path GameLibrary::TitleDir(uint32_t title_id) const {
  return root_ / fmt::format("{:08X}", title_id);
}

std::filesystem::path GameLibrary::EntryDir(const LibraryKey& key) const {
  return TitleDir(key.title_id) / xe::to_path(VersionToString(key.version));
}

bool GameLibrary::HasReleaseFolder(uint32_t title_id) const {
  std::error_code ec;
  for (const auto& sub : xe::filesystem::ListDirectories(TitleDir(title_id))) {
    const auto dir = sub.path / sub.name;
    if (ParseVersionFolder(sub.name) &&
        std::filesystem::exists(dir / "game.toml", ec)) {
      return true;
    }
  }
  return false;
}

std::filesystem::path GameLibrary::IconPath(const LibraryKey& key) const {
  return EntryDir(key) / "icon.png";
}

LibraryEntry* GameLibrary::Find(const LibraryKey& key) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const LibraryEntry& e) { return e.key() == key; });
  return it == entries_.end() ? nullptr : &*it;
}

const LibraryEntry* GameLibrary::Find(const LibraryKey& key) const {
  return const_cast<GameLibrary*>(this)->Find(key);
}

LibraryEntry* GameLibrary::FindByTitle(uint32_t title_id) {
  auto it = std::find_if(
      entries_.begin(), entries_.end(),
      [=](const LibraryEntry& e) { return e.title_id == title_id; });
  return it == entries_.end() ? nullptr : &*it;
}

const LibraryEntry* GameLibrary::FindByTitle(uint32_t title_id) const {
  return const_cast<GameLibrary*>(this)->FindByTitle(title_id);
}

LibraryEntry* GameLibrary::FindByPath(uint32_t title_id,
                                      const std::filesystem::path& path) {
  for (auto& entry : entries_) {
    if (entry.title_id != title_id) {
      continue;
    }
    for (const auto& entry_path : entry.paths) {
      if (entry_path.path == path) {
        return &entry;
      }
    }
  }
  return nullptr;
}

const LibraryEntry* GameLibrary::FindByPath(
    uint32_t title_id, const std::filesystem::path& path) const {
  return const_cast<GameLibrary*>(this)->FindByPath(title_id, path);
}

bool GameLibrary::ReadEntry(const std::filesystem::path& game_toml,
                            LibraryEntry& entry) const {
  toml::table tbl;
  try {
    tbl = toml::parse_file(xe::path_to_utf8(game_toml));
  } catch (const toml::parse_error& e) {
    XELOGE("GameLibrary: failed to parse {}: {}", xe::path_to_utf8(game_toml),
           e.description());
    return false;
  }

  auto title_id_str = tbl["title_id"].value<std::string>();
  if (!title_id_str) {
    XELOGW("GameLibrary: missing title_id, skipping {}",
           xe::path_to_utf8(game_toml));
    return false;
  }
  entry.title_id = xe::string_util::from_string<uint32_t>(*title_id_str, true);
  entry.name = tbl["name"].value_or(std::string());

  if (auto version_str = tbl["version"].value<std::string>()) {
    if (auto parsed = vfs::XexVersion::FromString(*version_str)) {
      entry.version = parsed->value();
    }
  }
  entry.last_played =
      static_cast<time_t>(tbl["last_played"].value_or(int64_t(0)));

  if (auto* paths = tbl["paths"].as_array()) {
    for (auto&& node : *paths) {
      auto* pt = node.as_table();
      if (!pt) {
        continue;
      }
      auto path_str = (*pt)["path"].value<std::string>();
      if (!path_str || path_str->empty()) {
        continue;
      }
      LibraryPath lp;
      lp.path = xe::to_path(*path_str);
      lp.is_default = (*pt)["default"].value_or(false);
      if (auto media_id_str = (*pt)["media_id"].value<std::string>()) {
        lp.media_id =
            xe::string_util::from_string<uint32_t>(*media_id_str, true);
      }
      lp.disc_number = static_cast<uint8_t>((*pt)["disc"].value_or(0));
      entry.paths.push_back(std::move(lp));
    }
  }

  NormalizeDefault(entry);
  return true;
}

std::vector<GameLibrary::Release> GameLibrary::SplitIntoReleases(
    const LibraryEntry& entry, bool& identified) const {
  std::vector<Release> result;
  identified = false;
  if (entry.paths.empty()) {
    result.push_back(Release{entry});
    return result;
  }

  std::vector<std::optional<GameFileInfo>> infos;
  infos.reserve(entry.paths.size());
  for (const auto& path : entry.paths) {
    infos.push_back(ReadGameFileInfo(path.path));
  }

  // A file that can't be read right now (moved, unplugged) must not look like
  // a release of its own, or a missing disc would split the title in two.
  // Those ride along with the first release we could identify. If none were
  // readable the entry stays whole and unidentified, to retry another time.
  identified = std::any_of(
      infos.begin(), infos.end(),
      [](const std::optional<GameFileInfo>& info) { return info.has_value(); });
  if (!identified) {
    result.push_back(Release{entry});
    return result;
  }

  // Group on version. Discs of a set share one, so they land together with
  // no special handling, and two releases of the game differ in it.
  //
  // The paths we could read come first, so that every release exists before
  // an unreadable path looks for one to join. Interleaving the two lets the
  // very first path be the unreadable one, with no release to attach it to.
  std::vector<uint8_t> best_disc;
  for (size_t i = 0; i < entry.paths.size(); ++i) {
    if (!infos[i]) {
      continue;
    }
    const GameFileInfo& info = *infos[i];
    auto it = std::find_if(result.begin(), result.end(), [&](const Release& r) {
      return r.entry.version == info.version;
    });
    if (it == result.end()) {
      Release part;
      part.entry.title_id = entry.title_id;
      part.entry.version = info.version;
      part.entry.name = entry.name;
      result.push_back(std::move(part));
      best_disc.push_back(0);
      it = std::prev(result.end());
    }
    const size_t slot = static_cast<size_t>(it - result.begin());

    // A release takes its art and names from its first disc, the one updates
    // are built against.
    const uint8_t disc =
        static_cast<uint8_t>(info.disc_number ? info.disc_number : 1);
    if (!best_disc[slot] || disc < best_disc[slot]) {
      best_disc[slot] = disc;
      it->icon = info.icon;
      it->title_name = info.title_name;
      it->display_name = info.display_name;
    }

    LibraryPath lp = entry.paths[i];
    lp.media_id = info.media_id;
    lp.disc_number = info.disc_number;
    it->entry.paths.push_back(std::move(lp));
  }

  // Then the ones we couldn't read. `identified` guarantees a release exists.
  for (size_t i = 0; i < entry.paths.size(); ++i) {
    if (!infos[i]) {
      result.front().entry.paths.push_back(entry.paths[i]);
    }
  }

  NameReleases(result, entry.name);

  for (auto& part : result) {
    NormalizeDefault(part.entry);
  }
  return result;
}

void GameLibrary::NameReleases(std::vector<Release>& releases,
                               const std::string& fallback) const {
  // An expansion shares its base game's title_name, so if every release says
  // the same thing that field cannot be what tells them apart.
  const bool titles_differ =
      std::any_of(releases.begin(), releases.end(), [&](const Release& r) {
        return r.title_name != releases.front().title_name;
      });
  const bool prefer_display = releases.size() > 1 && !titles_differ;

  for (auto& release : releases) {
    const std::string& first =
        prefer_display ? release.display_name : release.title_name;
    const std::string& second =
        prefer_display ? release.title_name : release.display_name;
    if (!first.empty()) {
      release.entry.name = first;
    } else if (!second.empty()) {
      release.entry.name = second;
    } else {
      release.entry.name = fallback;
    }
  }
}

void GameLibrary::PlaceIcon(const std::filesystem::path& dir,
                            const std::vector<uint8_t>& icon,
                            const std::filesystem::path& fallback_dir) const {
  std::error_code ec;
  if (!icon.empty()) {
    std::ofstream file(dir / "icon.png", std::ios::binary | std::ios::trunc);
    if (file) {
      file.write(reinterpret_cast<const char*>(icon.data()), icon.size());
      return;
    }
  }
  // ISO, XEX and ZAR carry no art, so the title's existing icon is all there
  // is for this release.
  const auto fallback = fallback_dir / "icon.png";
  if (std::filesystem::exists(fallback, ec)) {
    std::filesystem::copy_file(
        fallback, dir / "icon.png",
        std::filesystem::copy_options::overwrite_existing, ec);
  }
}

void GameLibrary::Load() {
  entries_.clear();
  split_titles_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(root_, ec)) {
    return;
  }

  const auto title_dirs = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(root_), std::regex("[0-9A-Fa-f]{8}"));

  for (const auto& title_dir : title_dirs) {
    const auto dir = title_dir.path / title_dir.name;

    // One version folder is proof the title is in the current layout, so the
    // game.toml orphaned at its root never gets opened. That costs only the
    // directory listing, which is what makes the orphan free to keep.
    //
    // Existing is what decides it, not loading. Re-deriving from the stale
    // root file would overwrite the folders that are fine.
    bool has_release_folder = false;
    for (const auto& sub : xe::filesystem::ListDirectories(dir)) {
      const auto version = ParseVersionFolder(sub.name);
      if (!version) {
        continue;
      }
      const auto game_toml = sub.path / sub.name / "game.toml";
      if (!std::filesystem::exists(game_toml, ec)) {
        continue;  // a bare folder is not a release, so migration still runs
      }
      has_release_folder = true;
      LibraryEntry entry;
      if (!ReadEntry(game_toml, entry)) {
        continue;
      }
      entry.version = *version;  // the folder name is what settles it
      entries_.push_back(std::move(entry));
    }
    if (!has_release_folder) {
      MigrateTitle(dir);
    }
  }
}

void GameLibrary::MigrateTitle(const std::filesystem::path& title_dir) {
  std::error_code ec;

  // Where the title's data actually is. A build that keyed releases on media
  // id left <title_id>/<media_id>/ folders holding newer data than the root
  // file, so those win outright when present. Never both, or the root file's
  // stale copy of the same paths would come along too.
  std::vector<std::filesystem::path> obsolete;
  LibraryEntry merged;
  bool found = false;
  for (const auto& sub :
       xe::filesystem::FilterByName(xe::filesystem::ListDirectories(title_dir),
                                    std::regex("[0-9A-Fa-f]{8}"))) {
    const auto sub_dir = sub.path / sub.name;
    LibraryEntry entry;
    if (!ReadEntry(sub_dir / "game.toml", entry)) {
      continue;
    }
    obsolete.push_back(sub_dir);

    // Pooled rather than migrated one folder at a time. Those folders were
    // keyed on media id, which gave each disc of a set its own, and two of
    // them now resolve to the same version folder. Splitting the pool is
    // also what puts such a set back together.
    found = true;
    merged.title_id = entry.title_id;
    if (merged.name.empty()) {
      merged.name = entry.name;
    }
    for (auto& disc : entry.paths) {
      const bool known = std::any_of(
          merged.paths.begin(), merged.paths.end(),
          [&](const LibraryPath& p) { return p.path == disc.path; });
      if (!known) {
        merged.paths.push_back(std::move(disc));
      }
    }
  }
  if (!found && !ReadEntry(title_dir / "game.toml", merged)) {
    return;
  }

  bool identified = false;
  auto parts = SplitIntoReleases(merged, identified);
  if (!identified) {
    // No readable path, so no version to name a folder with. Leave the title
    // where it is and try again once the files are back.
    entries_.push_back(std::move(merged));
    return;
  }
  if (parts.size() > 1) {
    split_titles_.push_back(merged.name.empty()
                                ? fmt::format("{:08X}", merged.title_id)
                                : merged.name);
  }
  for (auto& part : parts) {
    const auto release_dir = EntryDir(part.entry.key());
    std::filesystem::create_directories(release_dir, ec);
    // The title's own art is the fallback. A release whose package carries
    // its own gets that instead, so nothing is lost by not looking in the
    // media-id folders for the copies they were given.
    PlaceIcon(release_dir, part.icon, title_dir);
    WriteEntryAtomic(part.entry, release_dir);
    entries_.push_back(std::move(part.entry));
  }

  // Media-id folders are debris from a layout nothing reads any more, now
  // that what they held is written elsewhere. The root game.toml and icon
  // stay put, orphaned, for builds that predate versions.
  for (const auto& dir : obsolete) {
    std::filesystem::remove_all(dir, ec);
  }
}

bool GameLibrary::WriteEntryAtomic(const LibraryEntry& entry,
                                   const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot create {}: {}", xe::path_to_utf8(dir),
           ec.message());
    return false;
  }

  toml::array paths;
  for (const auto& p : entry.paths) {
    toml::table pt;
    if (p.disc_number) {
      pt.insert("disc", static_cast<int64_t>(p.disc_number));
    }
    pt.insert("media_id", fmt::format("{:08X}", p.media_id));
    pt.insert("path", PathToToml(p.path));
    if (p.is_default) {
      pt.insert("default", true);
    }
    pt.is_inline(true);
    paths.push_back(std::move(pt));
  }

  toml::table tbl;
  // Seconds since the epoch. Omitted while unset so a release that has never
  // been launched doesn't carry a field claiming it was.
  if (entry.last_played) {
    tbl.insert("last_played", static_cast<int64_t>(entry.last_played));
  }
  tbl.insert("name", entry.name);
  tbl.insert("paths", std::move(paths));
  tbl.insert("title_id", fmt::format("{:08X}", entry.title_id));
  tbl.insert("version", VersionToString(entry.version));

  const auto final_path = dir / "game.toml";
  const auto tmp_path = dir / "game.toml.tmp";
  {
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      XELOGE("GameLibrary: cannot open {}", xe::path_to_utf8(tmp_path));
      return false;
    }
    file << tbl << "\n";
    if (!file) {
      XELOGE("GameLibrary: write failed for {}", xe::path_to_utf8(tmp_path));
      return false;
    }
  }

  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot replace {}: {}", xe::path_to_utf8(final_path),
           ec.message());
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

bool GameLibrary::Upsert(LibraryEntry entry) {
  NormalizeDefault(entry);

  auto* existing = Find(entry.key());
  if (!WriteEntryAtomic(entry, EntryDir(entry.key()))) {
    return false;
  }

  if (existing) {
    *existing = std::move(entry);
  } else {
    entries_.push_back(std::move(entry));
  }
  return true;
}

bool GameLibrary::AddDisc(uint32_t title_id, const std::string& name,
                          const std::filesystem::path& path) {
  if (title_id == 0 || path.empty()) {
    return false;
  }

  // Which release this belongs to is the file's own version. Discs of a set
  // share one, so a disc joins its siblings without anything having to work
  // out that they are siblings.
  const auto info = ReadGameFileInfo(path);
  const LibraryKey key{title_id, info ? info->version : 0};
  LibraryEntry* existing = Find(key);

  LibraryEntry entry;
  if (existing) {
    entry = *existing;
  } else {
    entry.title_id = title_id;
    entry.version = key.version;
    entry.name = name;
    if (entry.name.empty()) {
      // A recorded disc swap supplies no name. Borrow the title's, or the
      // release shows up in the list as a nameless entry.
      if (const auto* sibling = FindByTitle(title_id)) {
        entry.name = sibling->name;
      }
    }
  }

  const bool known =
      std::any_of(entry.paths.begin(), entry.paths.end(),
                  [&](const LibraryPath& p) { return p.path == path; });
  if (known) {
    return false;
  }

  LibraryPath added;
  added.path = path;
  if (info) {
    added.media_id = info->media_id;
    added.disc_number = info->disc_number;
  }
  entry.paths.push_back(std::move(added));
  return Upsert(std::move(entry));
}

bool GameLibrary::MarkPlayed(const LibraryKey& key, time_t when) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  if (existing->last_played == when) {
    return true;  // same second, skip the rewrite
  }
  LibraryEntry entry = *existing;
  entry.last_played = when;
  return Upsert(std::move(entry));
}

bool GameLibrary::SetDefaultPath(const LibraryKey& key,
                                 const std::filesystem::path& path) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  const bool present =
      std::any_of(existing->paths.begin(), existing->paths.end(),
                  [&](const LibraryPath& p) { return p.path == path; });
  if (!present) {
    return false;
  }
  if (existing->default_path().path == path) {
    return true;  // already default, skip the rewrite
  }

  LibraryEntry entry = *existing;
  for (auto& p : entry.paths) {
    p.is_default = (p.path == path);
  }
  return Upsert(std::move(entry));
}

bool GameLibrary::RemovePath(const LibraryKey& key,
                             const std::filesystem::path& path) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  auto it = std::find_if(existing->paths.begin(), existing->paths.end(),
                         [&](const LibraryPath& p) { return p.path == path; });
  if (it == existing->paths.end()) {
    return false;
  }
  LibraryEntry entry = *existing;
  entry.paths.erase(entry.paths.begin() +
                    std::distance(existing->paths.begin(), it));
  return Upsert(std::move(entry));  // picks a new default if needed
}

bool GameLibrary::PruneMissingPaths(const LibraryKey& key) {
  auto* existing = Find(key);
  if (!existing) {
    return false;
  }
  LibraryEntry entry = *existing;
  const size_t before = entry.paths.size();
  std::error_code ec;
  entry.paths.erase(std::remove_if(entry.paths.begin(), entry.paths.end(),
                                   [&](const LibraryPath& p) {
                                     return !std::filesystem::exists(p.path,
                                                                     ec);
                                   }),
                    entry.paths.end());
  if (entry.paths.size() == before) {
    return false;
  }
  return Upsert(std::move(entry));  // picks a new default if needed
}

bool GameLibrary::SetIcon(const LibraryKey& key, std::span<const uint8_t> png) {
  const auto dir = EntryDir(key);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot create {}: {}", xe::path_to_utf8(dir),
           ec.message());
    return false;
  }

  const auto final_path = dir / "icon.png";
  const auto tmp_path = dir / "icon.png.tmp";
  {
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      XELOGE("GameLibrary: cannot open {}", xe::path_to_utf8(tmp_path));
      return false;
    }
    file.write(reinterpret_cast<const char*>(png.data()), png.size());
    if (!file) {
      XELOGE("GameLibrary: write failed for {}", xe::path_to_utf8(tmp_path));
      return false;
    }
  }

  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot replace {}: {}", xe::path_to_utf8(final_path),
           ec.message());
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

bool GameLibrary::Remove(const LibraryKey& key) {
  std::error_code ec;
  std::filesystem::remove_all(EntryDir(key), ec);
  if (ec) {
    XELOGE("GameLibrary: cannot remove {:08X}/{}: {}", key.title_id,
           VersionToString(key.version), ec.message());
    return false;
  }
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&](const LibraryEntry& e) { return e.key() == key; }),
      entries_.end());

  // The last release takes the title folder with it, orphaned root file
  // included, or an older build would go on listing a title the user removed.
  // Asks the filesystem rather than the loaded entries, so a release folder
  // that failed to load still keeps the title folder alive.
  if (!HasReleaseFolder(key.title_id)) {
    std::filesystem::remove_all(TitleDir(key.title_id), ec);
  }
  return true;
}

}  // namespace app
}  // namespace xe
