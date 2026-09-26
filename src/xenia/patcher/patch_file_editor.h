/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_PATCHER_PATCH_FILE_EDITOR_H_
#define XENIA_PATCHER_PATCH_FILE_EDITOR_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace patcher {

// Line-oriented editor for a single .patch.toml file that preserves original
// text/comments. Parses just enough metadata for a UI to render checkboxes,
// and exposes a single toggle-and-save operation.
class PatchFileEditor {
 public:
  struct PatchEntry {
    std::string name;
    std::string description;
    std::string author;
    bool is_enabled = false;
  };

  PatchFileEditor(std::string_view source_text,
                  std::filesystem::path storage_path);

  const std::vector<PatchEntry>& patches() const { return patches_; }
  const std::filesystem::path& storage_path() const { return storage_path_; }

  // Flips is_enabled for the patch at index and writes the file. Returns true
  // on success.
  bool SetEnabled(size_t index, bool value);

 private:
  bool UpdateEnabledLine(size_t patch_index, bool new_value);
  bool Save() const;

  std::filesystem::path storage_path_;
  std::vector<std::string> lines_;
  // Whether the source text ended with a line break, so saving keeps it.
  bool source_ends_with_newline_ = false;
  std::vector<PatchEntry> patches_;
};

}  // namespace patcher
}  // namespace xe

#endif  // XENIA_PATCHER_PATCH_FILE_EDITOR_H_
