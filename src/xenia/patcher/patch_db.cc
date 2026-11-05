/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xenia/patcher/patch_db.h"

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/embedded_bundle.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/memory.h"

#include "embedded_bundle_edge_patches.h"
#include "embedded_bundle_patches.h"

DEFINE_bool(apply_patches, true, "Enables custom patching functionality",
            "General");

namespace xe {
namespace patcher {

namespace {

const std::regex kPatchFilenameRegex("^[A-Fa-f0-9]{8}.*\\.patch\\.toml$");

// Hands every readable .patch.toml in dir to visit as (filename, contents).
template <typename Fn>
void ForEachPatchFileInDir(const std::filesystem::path& dir, Fn&& visit) {
  if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
    return;
  }
  for (const auto& fi : xe::filesystem::ListFiles(dir)) {
    std::string filename = xe::path_to_utf8(fi.name);
    if (!std::regex_match(filename, kPatchFilenameRegex)) {
      continue;
    }
    std::string contents = xe::filesystem::ReadAllText(fi.path / fi.name);
    if (contents.empty()) {
      continue;
    }
    visit(std::move(filename), std::move(contents));
  }
}

}  // namespace

PatchDB::PatchDB(std::filesystem::path patches_dir)
    : patches_dir_(std::move(patches_dir)) {}

PatchDB::~PatchDB() {}

PatchFileEntry PatchDB::ReadPatchFromString(
    const std::string& filename, std::string_view toml_content) const {
  PatchFileEntry patch_file;
  toml::parse_result patch_toml_fields;

  try {
    patch_toml_fields = toml::parse(toml_content);
  } catch (...) {
    XELOGE("PatchDB: Cannot parse patch file: {}", filename);
    patch_file.title_id = -1;
    return patch_file;
  };

  auto title_name = patch_toml_fields.get_as<std::string>("title_name");
  auto title_id = patch_toml_fields.get_as<std::string>("title_id");
  auto hashes_node = patch_toml_fields.get("hash");

  if (!title_name || !title_id || !hashes_node) {
    XELOGE("PatchDB: Cannot load patch file: {}", filename);
    patch_file.title_id = -1;
    return patch_file;
  }

  patch_file.title_id = strtoul(title_id->get().c_str(), NULL, 16);
  patch_file.title_name = title_name->get();
  ReadHashes(patch_file, hashes_node);

  auto patch_array = patch_toml_fields.get("patch");
  if (!patch_array || !patch_array->is_array()) {
    return patch_file;
  }

  for (const auto& patch_entry : *patch_array->as_array()) {
    if (!patch_entry.is_table()) {
      continue;
    }

    PatchInfoEntry patch = PatchInfoEntry();
    ReadPatchHeader(patch, patch_entry.as_table());
    patch_file.patch_info.push_back(patch);
  }
  return patch_file;
}

bool PatchDB::ReadPatchData(std::vector<PatchDataEntry>& patch_data,
                            const std::pair<std::string, PatchData> data_type,
                            const toml::table* patch_fields) const {
  auto patch_data_fields = patch_fields->get_as<toml::array>(data_type.first);
  if (!patch_data_fields) {
    return true;
  }

  for (const auto& patch_data_table : *patch_data_fields) {
    if (!patch_data_table.is_table()) {
      continue;
    }

    auto table = patch_data_table.as_table();
    auto address =
        static_cast<uint32_t>(table->get_as<int64_t>("address")->get());
    size_t alloc_size = (size_t)data_type.second.size;

    switch (data_type.second.type) {
      case PatchDataType::kBE8: {
        uint16_t value =
            static_cast<uint16_t>(table->get_as<int64_t>("value")->get());
        patch_data.push_back({address, PatchDataValue(alloc_size, value)});
        break;
      }
      case PatchDataType::kBE16: {
        uint16_t value =
            static_cast<uint16_t>(table->get_as<int64_t>("value")->get());
        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kBE32: {
        uint32_t value =
            static_cast<uint32_t>(table->get_as<int64_t>("value")->get());
        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kBE64: {
        uint64_t value = table->get_as<int64_t>("value")->get();
        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kF64: {
        const auto value_field = table->get("value");
        double value = 0.0;
        if (value_field->is_floating_point()) {
          value = value_field->as_floating_point()->get();
        }
        if (value_field->is_integer()) {
          value = static_cast<double>(value_field->as_integer()->get());
        }

        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kF32: {
        const auto value_field = table->get("value");
        float value = 0.0f;
        if (value_field->is_floating_point()) {
          value = static_cast<float>(value_field->as_floating_point()->get());
        }
        if (value_field->is_integer()) {
          value = static_cast<float>(value_field->as_integer()->get());
        }

        patch_data.push_back(
            {address, PatchDataValue(alloc_size, xe::byte_swap(value))});
        break;
      }
      case PatchDataType::kString: {
        std::string value = table->get_as<std::string>("value")->get();
        patch_data.push_back({address, PatchDataValue(value)});
        break;
      }
      case PatchDataType::kU16String: {
        std::u16string value =
            xe::to_utf16(table->get_as<std::string>("value")->get());
        patch_data.push_back({address, PatchDataValue(value)});
        break;
      }
      case PatchDataType::kByteArray: {
        std::vector<uint8_t> data;
        const std::string value = table->get_as<std::string>("value")->get();

        bool success = string_util::hex_string_to_array(data, value);
        if (!success) {
          XELOGW("PatchDB: Cannot convert hex string to byte array! Skipping",
                 address);
          return false;
        }
        patch_data.push_back({address, PatchDataValue(data)});
        break;
      }
      default: {
        XELOGW("PatchDB: Unknown patch data type for address {:08X}! Skipping",
               address);
        return false;
      }
    }
  }
  return true;
}

std::vector<PatchFileEntry> PatchDB::GetTitlePatches(
    const uint32_t title_id, const std::optional<uint64_t> hash) {
  std::vector<PatchFileEntry> title_patches;
  if (!cvars::apply_patches) {
    return title_patches;
  }

  for (auto& file : EnumeratePatchesForTitle(patches_dir_, title_id)) {
    if (std::ranges::find(file.entry.hashes, hash) ==
        file.entry.hashes.cend()) {
      continue;
    }
    file.entry.filename = std::move(file.filename);
    title_patches.push_back(std::move(file.entry));
  }

  return title_patches;
}

void PatchDB::ReadHashes(PatchFileEntry& patch_entry,
                         const toml::node* hashes_node) const {
  auto add_hash = [&patch_entry](const toml::node* hash_node) {
    if (!hash_node->is_string()) {
      return;
    }

    const auto string_hash = hash_node->as_string()->get();
    if (string_hash.empty()) {
      return;
    }

    patch_entry.hashes.push_back(
        xe::string_util::from_string<uint64_t>(string_hash, true));
  };

  if (hashes_node->is_value()) {
    add_hash(hashes_node);
  }

  if (hashes_node->is_array()) {
    for (const auto& hash_entry : *hashes_node->as_array()) {
      add_hash(&hash_entry);
    }
  }
}

void PatchDB::ReadPatchHeader(PatchInfoEntry& patch_info,
                              const toml::table* patch_fields) const {
  std::string patch_name = {};
  std::string patch_desc = {};
  std::string patch_author = {};
  bool is_enabled = false;

  if (patch_fields->contains("name")) {
    patch_name = patch_fields->get_as<std::string>("name")->get();
  }
  if (patch_fields->contains("desc")) {
    patch_desc = patch_fields->get_as<std::string>("desc")->get();
  }
  if (patch_fields->contains("author")) {
    patch_author = patch_fields->get_as<std::string>("author")->get();
  }
  if (patch_fields->contains("is_enabled")) {
    is_enabled = patch_fields->get_as<bool>("is_enabled")->get();
  }

  patch_info.id = 0;  // Todo(Gliniak): Implement id for future GUI stuff
  patch_info.patch_name = patch_name;
  patch_info.patch_desc = patch_desc;
  patch_info.patch_author = patch_author;
  patch_info.is_enabled = is_enabled;

  // Iterate through all available data sizes
  for (const auto& patch_data_type : patch_data_types_size_) {
    bool success =
        ReadPatchData(patch_info.patch_data, patch_data_type, patch_fields);

    if (!success) {
      XELOGE("PatchDB: Cannot read patch {}", patch_name);
      break;
    }
  }
}

namespace {

const std::regex kPatchHeaderRegex(R"(^\s*\[\[\s*patch\s*\]\]\s*(#[^\n]*)?$)");
const std::regex kTableHeaderRegex(R"(^\s*\[)");
const std::regex kEnabledRegex(
    R"((^\s*is_enabled\s*=\s*)(true|false)([^\n]*)$)");

// A patch file's lines before its first [[patch]], then each patch's lines.
struct PatchFileText {
  std::vector<std::string> header;
  std::vector<std::vector<std::string>> patches;
};

PatchFileText SplitPatchFile(std::string_view text) {
  PatchFileText file;
  std::stringstream stream{std::string(text)};
  std::string line;
  while (std::getline(stream, line)) {
    if (std::regex_match(line, kPatchHeaderRegex)) {
      file.patches.emplace_back();
    }
    (file.patches.empty() ? file.header : file.patches.back())
        .push_back(std::move(line));
  }
  return file;
}

// Sets is_enabled in the patch's own table, adding it when the patch has none.
void SetPatchEnabled(std::vector<std::string>& patch, bool enabled) {
  const std::string value = enabled ? "true" : "false";
  for (size_t i = 1; i < patch.size(); ++i) {
    if (std::regex_search(patch[i], kTableHeaderRegex)) {
      break;
    }
    std::smatch match;
    if (std::regex_match(patch[i], match, kEnabledRegex)) {
      patch[i] = match[1].str() + value + match[3].str();
      return;
    }
  }
  const bool crlf = patch[0].ends_with('\r');
  patch.insert(patch.begin() + 1,
               "    is_enabled = " + value + (crlf ? "\r" : ""));
}

// Decompresses + parses the embedded bundles once and indexes by title_id.
// The bundles are compiled in and immutable so the cache lives forever.
struct BundledIndex {
  std::vector<PatchSourceFile> all;
  std::unordered_map<uint32_t, std::vector<size_t>> by_title;
};

const BundledIndex& GetBundledIndex() {
  static const BundledIndex index = []() {
    BundledIndex idx;
    PatchDB scratch{std::filesystem::path()};
    // game-patches goes first so an edge file of the same name merges into it
    // with edge's patches winning.
    using Bundle = std::pair<const unsigned char*, size_t>;
    for (const auto& [bundle_data, bundle_size] :
         {Bundle{xe::embedded_bundle_patches::kBundleData,
                 xe::embedded_bundle_patches::kBundleSize},
          Bundle{xe::embedded_bundle_edge_patches::kBundleData,
                 xe::embedded_bundle_edge_patches::kBundleSize}}) {
      EmbeddedBundle bundle(bundle_data, bundle_size);
      if (!bundle.ok()) {
        XELOGE("BundledIndex: bundle decompress failed");
        continue;
      }
      bundle.ForEach([&](std::string_view name, std::string_view data) {
        std::string filename(name);
        if (!filename.ends_with(".patch.toml")) {
          return;
        }
        PatchFileEntry entry = scratch.ReadPatchFromString(filename, data);
        if (entry.title_id == static_cast<uint32_t>(-1)) {
          return;
        }
        auto same =
            std::ranges::find(idx.all, filename, &PatchSourceFile::filename);
        if (same != idx.all.end() && same->entry.title_id == entry.title_id) {
          if (auto merged = MergePatchFiles(data, same->toml_content, false)) {
            same->toml_content = std::move(*merged);
          } else {
            XELOGW("PatchDB: Bundled {} doesn't merge, using edge's", filename);
            same->toml_content = std::string(data);
          }
          same->entry =
              scratch.ReadPatchFromString(filename, same->toml_content);
          return;
        }
        PatchSourceFile file;
        file.filename = std::move(filename);
        file.toml_content = std::string(data);
        file.entry = std::move(entry);
        idx.all.push_back(std::move(file));
      });
    }
    std::sort(idx.all.begin(), idx.all.end(),
              [](const PatchSourceFile& a, const PatchSourceFile& b) {
                return a.filename < b.filename;
              });
    for (size_t i = 0; i < idx.all.size(); ++i) {
      idx.by_title[idx.all[i].entry.title_id].push_back(i);
    }
    return idx;
  }();
  return index;
}

std::vector<PatchSourceFile> EnumerateBundledPatchesForTitle(
    uint32_t title_id) {
  std::vector<PatchSourceFile> out;
  const auto& idx = GetBundledIndex();
  auto it = idx.by_title.find(title_id);
  if (it == idx.by_title.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (size_t i : it->second) {
    out.push_back(idx.all[i]);
  }
  return out;
}

std::vector<PatchSourceFile> EnumerateLocalPatchesForTitle(
    const std::filesystem::path& patches_dir, uint32_t title_id) {
  std::vector<PatchSourceFile> out;
  // The title id inside the file decides ownership, not the filename prefix.
  PatchDB scratch{std::filesystem::path()};
  ForEachPatchFileInDir(
      patches_dir, [&](std::string filename, std::string contents) {
        PatchFileEntry entry = scratch.ReadPatchFromString(filename, contents);
        if (entry.title_id != title_id) {
          return;
        }
        PatchSourceFile file;
        file.filename = std::move(filename);
        file.toml_content = std::move(contents);
        file.entry = std::move(entry);
        out.push_back(std::move(file));
      });
  std::sort(out.begin(), out.end(),
            [](const PatchSourceFile& a, const PatchSourceFile& b) {
              return a.filename < b.filename;
            });
  return out;
}

}  // namespace

std::vector<PatchSourceFile> EnumeratePatchesForTitle(
    const std::filesystem::path& patches_dir, uint32_t title_id) {
  std::vector<PatchSourceFile> files =
      EnumerateBundledPatchesForTitle(title_id);
  const size_t bundled_count = files.size();
  PatchDB scratch{std::filesystem::path()};
  for (auto& local : EnumerateLocalPatchesForTitle(patches_dir, title_id)) {
    const auto bundled_end = files.begin() + bundled_count;
    auto bundled = std::find_if(files.begin(), bundled_end,
                                [&local](const PatchSourceFile& file) {
                                  return file.filename == local.filename;
                                });
    if (bundled == bundled_end) {
      files.push_back(std::move(local));
      continue;
    }
    auto merged =
        MergePatchFiles(bundled->toml_content, local.toml_content, true);
    if (!merged) {
      XELOGW("PatchDB: {} doesn't merge with its bundled file, using it as is",
             local.filename);
      *bundled = std::move(local);
      continue;
    }
    bundled->toml_content = std::move(*merged);
    bundled->entry =
        scratch.ReadPatchFromString(bundled->filename, bundled->toml_content);
  }
  return files;
}

std::optional<std::string> MergePatchFiles(std::string_view base,
                                           std::string_view extra,
                                           bool keep_extra_enabled) {
  PatchDB scratch{std::filesystem::path()};
  const PatchFileEntry base_entry =
      scratch.ReadPatchFromString(std::string(), base);
  const PatchFileEntry extra_entry =
      scratch.ReadPatchFromString(std::string(), extra);
  PatchFileText base_text = SplitPatchFile(base);
  const PatchFileText extra_text = SplitPatchFile(extra);
  // Pairing goes by the parsed patches so each has to be one [[patch]] block.
  auto lines_up = [](const PatchFileEntry& entry, const PatchFileText& text) {
    return entry.title_id != static_cast<uint32_t>(-1) &&
           entry.patch_info.size() == text.patches.size();
  };
  if (!lines_up(base_entry, base_text) || !lines_up(extra_entry, extra_text)) {
    return std::nullopt;
  }

  // The nth patch of a name pairs with the nth patch of that name in extra.
  std::map<std::string, std::vector<size_t>> extra_by_name;
  for (size_t i = 0; i < extra_entry.patch_info.size(); ++i) {
    extra_by_name[extra_entry.patch_info[i].patch_name].push_back(i);
  }
  std::vector<bool> paired(extra_entry.patch_info.size());
  std::map<std::string, size_t> seen;
  std::vector<std::string> lines = std::move(base_text.header);
  for (size_t i = 0; i < base_text.patches.size(); ++i) {
    const PatchInfoEntry& patch = base_entry.patch_info[i];
    auto& patch_lines = base_text.patches[i];
    const size_t nth = seen[patch.patch_name]++;
    if (auto it = extra_by_name.find(patch.patch_name);
        it != extra_by_name.end() && nth < it->second.size()) {
      const size_t j = it->second[nth];
      paired[j] = true;
      if (keep_extra_enabled) {
        SetPatchEnabled(patch_lines, extra_entry.patch_info[j].is_enabled);
      }
    }
    lines.insert(lines.end(), patch_lines.begin(), patch_lines.end());
  }
  for (size_t j = 0; j < extra_text.patches.size(); ++j) {
    if (!paired[j]) {
      lines.insert(lines.end(), extra_text.patches[j].begin(),
                   extra_text.patches[j].end());
    }
  }

  std::string merged;
  for (const auto& line : lines) {
    merged += line;
    merged += '\n';
  }
  return merged;
}

std::string PatchDisplayName(const PatchSourceFile& file) {
  std::string display = file.filename;
  if (auto dash = display.find(" - "); dash != std::string::npos) {
    display = display.substr(dash + 3);
  }
  if (auto suffix = display.rfind(".patch.toml"); suffix != std::string::npos) {
    display = display.substr(0, suffix);
  }
  return display;
}

}  // namespace patcher
}  // namespace xe
