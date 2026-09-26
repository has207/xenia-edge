/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/patcher/patch_file_editor.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wabsolute-value"
#endif
#include "third_party/tomlplusplus/toml.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

namespace xe {
namespace patcher {

PatchFileEditor::PatchFileEditor(std::string_view source_text,
                                 std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path)) {
  {
    std::stringstream ss{std::string(source_text)};
    std::string line;
    while (std::getline(ss, line)) {
      lines_.push_back(std::move(line));
    }
    source_ends_with_newline_ =
        !source_text.empty() && source_text.back() == '\n';
  }

  toml::table root;
  try {
    root = toml::parse(source_text);
  } catch (const std::exception& e) {
    XELOGE("PatchFileEditor: failed to parse {}: {}",
           xe::path_to_utf8(storage_path_), e.what());
    return;
  }

  auto* arr = root["patch"].as_array();
  if (!arr) {
    return;
  }
  for (const auto& node : *arr) {
    auto* tbl = node.as_table();
    if (!tbl) {
      continue;
    }
    PatchEntry info;
    if (auto v = (*tbl)["name"].value<std::string>()) {
      info.name = *v;
    }
    if (auto v = (*tbl)["desc"].value<std::string>()) {
      info.description = *v;
    }
    if (auto v = (*tbl)["author"].value<std::string>()) {
      info.author = *v;
    }
    if (auto v = (*tbl)["is_enabled"].value<bool>()) {
      info.is_enabled = *v;
    }
    patches_.push_back(std::move(info));
  }
}

bool PatchFileEditor::UpdateEnabledLine(size_t patch_index, bool new_value) {
  static const std::regex patch_header(R"(^\s*\[\[patch\]\]\s*$)");
  static const std::regex section_header(R"(^\s*\[([^\[\]]+)\]\s*$)");
  static const std::regex is_enabled(R"(^\s*is_enabled\s*=\s*(.+))");
  static const std::regex comment_line(R"(^\s*#.*)");
  // [^\n] so a trailing \r from a CRLF source survives into m[3]; . excludes
  // it.
  static const std::regex value_regex(
      R"((^\s*is_enabled\s*=\s*)(true|false)([^\n]*)$)");

  std::vector<size_t> enabled_lines;
  bool in_patch = false;
  for (size_t i = 0; i < lines_.size(); ++i) {
    const std::string& line = lines_[i];
    if (std::regex_match(line, comment_line)) {
      continue;
    }
    if (std::regex_match(line, patch_header)) {
      in_patch = true;
      continue;
    }
    if (std::regex_match(line, section_header)) {
      in_patch = false;
      continue;
    }
    if (in_patch && std::regex_search(line, is_enabled)) {
      enabled_lines.push_back(i);
    }
  }
  if (patch_index >= enabled_lines.size()) {
    return false;
  }
  std::smatch m;
  std::string& line = lines_[enabled_lines[patch_index]];
  if (!std::regex_match(line, m, value_regex)) {
    return false;
  }
  line = m[1].str() + (new_value ? "true" : "false") + m[3].str();
  return true;
}

bool PatchFileEditor::Save() const {
  std::error_code ec;
  std::filesystem::create_directories(storage_path_.parent_path(), ec);
  std::ofstream out(storage_path_, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    XELOGE("PatchFileEditor: failed to write {}",
           xe::path_to_utf8(storage_path_));
    return false;
  }
  for (size_t i = 0; i < lines_.size(); ++i) {
    out << lines_[i];
    if (i + 1 < lines_.size()) {
      out << "\n";
    }
  }
  // The lines keep the \r of a CRLF source, so the last line must get its \n
  // back too - a file ending in a lone \r is rejected by the TOML parser, and
  // the whole patch file then vanishes from the list on the next launch.
  if (!lines_.empty() &&
      (source_ends_with_newline_ ||
       (!lines_.back().empty() && lines_.back().back() == '\r'))) {
    out << "\n";
  }
  return true;
}

bool PatchFileEditor::SetEnabled(size_t index, bool value) {
  if (storage_path_.empty()) {
    XELOGE("PatchFileEditor: no storage path; cannot save");
    return false;
  }
  if (!UpdateEnabledLine(index, value)) {
    XELOGE("PatchFileEditor: failed to flip is_enabled for patch #{}",
           index + 1);
    return false;
  }
  if (!Save()) {
    return false;
  }
  if (index < patches_.size()) {
    patches_[index].is_enabled = value;
  }
  return true;
}

}  // namespace patcher
}  // namespace xe
