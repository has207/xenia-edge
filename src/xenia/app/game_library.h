/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_LIBRARY_H_
#define XENIA_APP_GAME_LIBRARY_H_

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xe {
namespace app {

// What a game file says about itself. Empty when the file can't be read at
// all, which is different from a game that genuinely has no media id:
// homebrew commonly reports zero.
struct GameFileInfo {
  uint32_t media_id = 0;
  // Packed XEX version. One value for a whole release, and different between
  // releases, which is what makes it the key.
  uint32_t version = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
  // Title art the file carries. Only STFS containers embed one, so this is
  // empty for ISO, XEX and ZAR, which is also why the names below are.
  std::vector<uint8_t> icon;
  // What the package calls itself. `title_name` is the game it belongs to,
  // which an expansion shares with its base game. `display_name` is the
  // content's own, which is what tells the two apart.
  std::string title_name;
  std::string display_name;
};
std::optional<GameFileInfo> ReadGameFileInfo(const std::filesystem::path& path);

// "major.minor.build.qfe", the form used for release folder names and for
// showing a version to the user.
std::string VersionToString(uint32_t version);

// One launchable location for a title (a disc, or an alternate copy).
struct LibraryPath {
  std::filesystem::path path;
  bool is_default = false;
  // What this file reports. Recorded rather than re-read, so a title update
  // can be matched against the discs while they are offline.
  uint32_t media_id = 0;
  uint8_t disc_number = 0;
};

// Identifies one release of a title. The same game can be present more than
// once, as a disc rip and a download or as two regions, and those are not
// interchangeable, so each gets an entry.
//
// Version discriminates them because it is one value for the whole release,
// which makes the key a pure function of any disc of it. A media id would
// not: a multi-disc release has one per disc.
struct LibraryKey {
  uint32_t title_id = 0;
  uint32_t version = 0;

  bool operator==(const LibraryKey& other) const {
    return title_id == other.title_id && version == other.version;
  }
  bool operator!=(const LibraryKey& other) const { return !(*this == other); }
};

// A single installed release, with its own icon: two releases of a game can
// ship different art.
//
// Always at <root>/<title_id>/<version>/, with no fallback to the title
// folder root. Builds that predate this read <root>/<title_id>/game.toml, so
// migration leaves that file and its icon where they are, orphaned but
// readable. Nothing here writes to them again.
struct LibraryEntry {
  uint32_t title_id = 0;
  // Names the release folder. Zero is a real value and its own bucket.
  uint32_t version = 0;
  // Derived from the files when the release is identified, then editable.
  // Two releases of a game can be identical in every field their packages
  // carry, leaving a rename the only way to tell them apart.
  std::string name;
  std::vector<LibraryPath> paths;  // exactly one is default when non-empty
  // When this release was last launched. Per release because the profile
  // GPD only records a time per title, which cannot say which of them ran.
  time_t last_played = 0;

  LibraryKey key() const { return {title_id, version}; }

  // The flagged default, else the first. Precondition: paths is non-empty.
  const LibraryPath& default_path() const;
};

// Catalog of installed games, sharded one folder per release under
// storage_root/library, so updates touch only that release.
class GameLibrary {
 public:
  explicit GameLibrary(std::filesystem::path root);

  // Scans the library root and (re)loads every release's metadata. A title
  // still in an older layout is migrated once, which reads its files and so
  // splits any that turn out to hold more than one release. A title that
  // already has a version folder costs only a directory listing, so the
  // orphaned root game.toml is never parsed again.
  void Load();

  // Titles that Load() separated into several releases, for telling the user
  // once. Cleared by the next Load().
  const std::vector<std::string>& split_titles() const { return split_titles_; }

  const std::vector<LibraryEntry>& entries() const { return entries_; }

  LibraryEntry* Find(const LibraryKey& key);
  const LibraryEntry* Find(const LibraryKey& key) const;
  // First release of a title, for callers holding only a title id (a profile
  // GPD, the running title). Null when the title isn't present.
  LibraryEntry* FindByTitle(uint32_t title_id);
  const LibraryEntry* FindByTitle(uint32_t title_id) const;
  // The release holding `path`, for a caller that wants the key after an add
  // without re-reading the file.
  LibraryEntry* FindByPath(uint32_t title_id,
                           const std::filesystem::path& path);
  const LibraryEntry* FindByPath(uint32_t title_id,
                                 const std::filesystem::path& path) const;

  // Inserts or replaces a release and persists just its game.toml.
  // Normalizes the default-path marker before writing.
  bool Upsert(LibraryEntry entry);

  // Records `path` as a disc of the release it belongs to (creating it with
  // `name` if absent), deduped, and persists. Which release that is comes
  // from the file's own version. False on a dup, title_id 0, or empty path.
  bool AddDisc(uint32_t title_id, const std::string& name,
               const std::filesystem::path& path);

  // Promotes `path` to the release's default (double-click launch) disc and
  // persists. No-op if it is already default. False if the release or path is
  // unknown.
  bool SetDefaultPath(const LibraryKey& key, const std::filesystem::path& path);

  // Drops `path` from `key` and persists. False if it has no such path.
  bool RemovePath(const LibraryKey& key, const std::filesystem::path& path);

  // Stamps `key` as launched at `when` and persists. False if it is unknown.
  bool MarkPlayed(const LibraryKey& key, time_t when);

  // Drops any of the release's discs whose file no longer exists and persists.
  // False if the release is unknown or nothing was missing.
  bool PruneMissingPaths(const LibraryKey& key);

  // Writes icon.png into the release's folder.
  bool SetIcon(const LibraryKey& key, std::span<const uint8_t> png);
  std::filesystem::path IconPath(const LibraryKey& key) const;

  // Removes a release's folder and drops it from the in-memory list.
  bool Remove(const LibraryKey& key);

 private:
  // <root>/<title_id>, shared by every release of a title.
  std::filesystem::path TitleDir(uint32_t title_id) const;
  // <root>/<title_id>/<version>. A pure function of the key, so a release is
  // always where the key says and never has to be searched for.
  std::filesystem::path EntryDir(const LibraryKey& key) const;
  // Whether any release folder survives under a title, which is what decides
  // if the title folder is still wanted. A game.toml has to be there, but it
  // does not have to parse: one we failed to read still holds the folder.
  bool HasReleaseFolder(uint32_t title_id) const;
  // Moves one title out of an older layout into <title_id>/<version>/, and
  // appends what it found to entries_. Reads every path, so it also separates
  // a title that turns out to hold more than one release.
  void MigrateTitle(const std::filesystem::path& title_dir);
  bool WriteEntryAtomic(const LibraryEntry& entry,
                        const std::filesystem::path& dir);
  // Reads one game.toml. The folder name is what settles an entry's version,
  // so the parsed value is only a fallback for a hand-moved folder.
  bool ReadEntry(const std::filesystem::path& game_toml,
                 LibraryEntry& entry) const;
  // A release found among an entry's paths, with the art its own files carry
  // rather than the art the title as a whole had.
  struct Release {
    LibraryEntry entry;
    std::vector<uint8_t> icon;
    // Straight from the package, before deciding which one to show.
    std::string title_name;
    std::string display_name;
  };
  // Names each release from its own package. When they all report the same
  // title, which an expansion shares with its base game, the display name is
  // what distinguishes them, so that wins instead. `fallback` is the name the
  // title already had, for releases whose files say nothing.
  void NameReleases(std::vector<Release>& releases,
                    const std::string& fallback) const;
  // Separates `entry` into releases, one per version. Discs of a set share a
  // version and so stay together with no special handling. `identified` is
  // false when no path could be read, leaving the result worth retrying.
  std::vector<Release> SplitIntoReleases(const LibraryEntry& entry,
                                         bool& identified) const;
  // Writes `icon` into `dir`, or copies the art already at `fallback_dir`
  // when the files carry none, which is the case for ISO, XEX and ZAR.
  void PlaceIcon(const std::filesystem::path& dir,
                 const std::vector<uint8_t>& icon,
                 const std::filesystem::path& fallback_dir) const;

  std::filesystem::path root_;
  std::vector<LibraryEntry> entries_;
  std::vector<std::string> split_titles_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_LIBRARY_H_
