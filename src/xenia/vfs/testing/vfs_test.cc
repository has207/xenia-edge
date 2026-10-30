/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/gdfx_util.h"
#include "xenia/vfs/xbe_metadata.h"
#include "xenia/vfs/xbox_original_data.h"
#include "xenia/vfs/xex_metadata.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::vfs::test {

TEST_CASE("STFS Decode date and time", "[stfs_decode]") {
  SECTION("10 June 2022 19:46:00 UTC - Decode") {
    const uint16_t date = 0x54CA;
    const uint16_t time = 0x9DBD;
    const uint64_t result = 132993639580000000;

    const uint64_t timestamp = decode_fat_timestamp(date, time);

    REQUIRE(timestamp == result);
  }
}

// Writes a directory entry at |offset| in |image|.
static void WriteGdfxEntry(std::vector<uint8_t>& image, size_t offset,
                           uint16_t node_l, uint16_t node_r, uint32_t sector,
                           uint32_t length, const char* name) {
  const uint8_t name_length = uint8_t(std::strlen(name));
  std::memcpy(&image[offset + 0], &node_l, sizeof(node_l));
  std::memcpy(&image[offset + 2], &node_r, sizeof(node_r));
  std::memcpy(&image[offset + 4], &sector, sizeof(sector));
  std::memcpy(&image[offset + 8], &length, sizeof(length));
  image[offset + 12] = 0;
  image[offset + 13] = name_length;
  std::memcpy(&image[offset + 14], name, name_length);
}

TEST_CASE("GDFX directory padding", "[gdfx]") {
  SECTION("Padding that starts a sector stands in for no entry") {
    REQUIRE(GdfxPaddedEntryOrdinal(0) == 0);
    REQUIRE(GdfxPaddedEntryOrdinal(0x200) == 0);
  }

  SECTION("Padding after an entry stands in for the next sector's first") {
    REQUIRE(GdfxPaddedEntryOrdinal(1) == 0x200);
    REQUIRE(GdfxPaddedEntryOrdinal(0x1F0) == 0x200);
    REQUIRE(GdfxPaddedEntryOrdinal(0xFFFF) == 0x10000);
  }

  SECTION("A lookup follows padding to the next sector") {
    // A two sector root directory at sector 1 whose second entry didn't fit
    // in the first sector.
    std::vector<uint8_t> image(8 * kGdfxSectorSize);
    const size_t root = kGdfxSectorSize;
    WriteGdfxEntry(image, root, 0, 0x1F0, 6, 0x10, "a.bin");
    std::memset(&image[root + 0x1F0 * 4], 0xFF, kGdfxSectorSize - 0x1F0 * 4);
    WriteGdfxEntry(image, root + kGdfxSectorSize, 0, 0, 5, 0x100,
                   "default.xex");

    const GdfxPartitionInfo partition = {0, 1, 2 * kGdfxSectorSize};
    auto location =
        GdfxFindFile(image.data(), image.size(), partition, "default.xex");
    REQUIRE(location);
    REQUIRE(location->offset == 5 * kGdfxSectorSize);
    REQUIRE(location->length == 0x100);
  }

  SECTION("A lookup in an empty directory finds nothing") {
    std::vector<uint8_t> image(4 * kGdfxSectorSize);
    std::memset(&image[kGdfxSectorSize], 0xFF, kGdfxSectorSize);
    const GdfxPartitionInfo partition = {0, 1, kGdfxSectorSize};
    REQUIRE(!GdfxFindFile(image.data(), image.size(), partition, "a.bin"));
  }
}

template <typename T>
static void Put(std::vector<uint8_t>& data, size_t offset, T value) {
  std::memcpy(&data[offset], &value, sizeof(value));
}

// An executable with a certificate and a 4x4 title image in |texel_format|.
static std::vector<uint8_t> MakeXbe(uint32_t texel_format) {
  constexpr uint32_t kBase = 0x10000;
  std::vector<uint8_t> xbe(0x1000 + 0x20 + 4 * 4 * 4);
  std::memcpy(&xbe[0], "XBEH", 4);
  Put<uint32_t>(xbe, 0x104, kBase);
  Put<uint32_t>(xbe, 0x108, 0x1000);
  Put<uint32_t>(xbe, 0x118, kBase + 0x200);
  Put<uint32_t>(xbe, 0x11C, 1);
  Put<uint32_t>(xbe, 0x120, kBase + 0x300);
  // The certificate.
  Put<uint32_t>(xbe, 0x200 + 0x8, 0x4C410005);
  const char16_t name[] = u"Jedi";
  std::memcpy(&xbe[0x200 + 0xC], name, sizeof(name));
  Put<uint32_t>(xbe, 0x200 + 0xA8, 1);
  Put<uint32_t>(xbe, 0x200 + 0xAC, 6);
  // The title image section and its name.
  Put<uint32_t>(xbe, 0x300 + 0xC, 0x1000);
  Put<uint32_t>(xbe, 0x300 + 0x10, 0x20 + 4 * 4 * 4);
  Put<uint32_t>(xbe, 0x300 + 0x14, kBase + 0x400);
  std::memcpy(&xbe[0x400], "$$XTIMAGE", 10);
  // One 4x4 texture, its texels right after the bundle's header.
  std::memcpy(&xbe[0x1000], "XPR0", 4);
  Put<uint32_t>(xbe, 0x1004, 0x20 + 4 * 4 * 4);
  Put<uint32_t>(xbe, 0x1008, 0x20);
  Put<uint32_t>(xbe, 0x1000 + 24, (texel_format << 8) | (2 << 20) | (2 << 24));
  std::memset(&xbe[0x1020], 0x80, 4 * 4 * 4);
  return xbe;
}

TEST_CASE("XBE metadata", "[xbe]") {
  SECTION("The certificate describes the game") {
    const auto xbe = MakeXbe(0x12);
    auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
    REQUIRE(m);
    REQUIRE(m->title_id == 0x4C410005);
    REQUIRE(m->title_name == "Jedi");
    REQUIRE(m->disc_number == 1);
    REQUIRE(m->version == 6);
  }

  SECTION("A broken surrogate pair doesn't stop the name being read") {
    auto xbe = MakeXbe(0x12);
    Put<uint16_t>(xbe, 0x200 + 0xC + 3 * 2, 0xD800);
    auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
    REQUIRE(m);
    REQUIRE(m->title_name == "Jed\xEF\xBF\xBD");
  }

  SECTION("The title image comes back as PNG") {
    for (uint32_t format : {0x06u, 0x12u}) {
      const auto xbe = MakeXbe(format);
      auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
      REQUIRE(m);
      REQUIRE(m->icon_png.size() > 8);
      REQUIRE(std::memcmp(m->icon_png.data(), "\x89PNG", 4) == 0);
    }
  }

  SECTION("A format it doesn't decode leaves no icon") {
    const auto xbe = MakeXbe(0x0B);
    auto m = ExtractXbeMetadata(xbe.data(), xbe.size());
    REQUIRE(m);
    REQUIRE(m->icon_png.empty());
  }

  SECTION("Anything else is not an executable") {
    auto xbe = MakeXbe(0x12);
    xbe[0] = 0;
    REQUIRE(!ExtractXbeMetadata(xbe.data(), xbe.size()));
  }
}

// UTF-16 text behind the byte order mark the Xbox writes ahead of it.
static std::vector<uint8_t> Utf16Text(std::u16string_view text) {
  std::vector<uint8_t> bytes = {0xFF, 0xFE};
  for (char16_t c : text) {
    bytes.push_back(uint8_t(c));
    bytes.push_back(uint8_t(c >> 8));
  }
  return bytes;
}

// A ContentMeta.xbx whose first section is |section|.
static std::vector<uint8_t> MakeContentMeta(
    const std::vector<uint8_t>& section) {
  std::vector<uint8_t> meta(0x88);
  std::memcpy(&meta[0x14], "XCSF", 4);
  Put<uint32_t>(meta, 0x34, 0x88);
  Put<uint32_t>(meta, 0x38, uint32_t(section.size()));
  meta.insert(meta.end(), section.begin(), section.end());
  return meta;
}

TEST_CASE("Original Xbox save and content names", "[xbox_original]") {
  SECTION("A save is named by its SaveMeta") {
    const auto meta = Utf16Text(u"Name=PRINCE_01\r\nNoCopy=1\r\n");
    REQUIRE(ReadSaveMetaName(meta) == "PRINCE_01");
  }

  SECTION("A SaveMeta without a name names nothing") {
    REQUIRE(ReadSaveMetaName({}).empty());
    REQUIRE(ReadSaveMetaName(Utf16Text(u"NoCopy=1\r\n")).empty());
  }

  SECTION("The first two bytes are skipped whatever they hold") {
    auto meta = Utf16Text(u"Name=Career\r\n");
    meta[0] = 'X';
    meta[1] = 0;
    REQUIRE(ReadSaveMetaName(meta) == "Career");
  }

  SECTION("A broken surrogate pair doesn't stop the name being read") {
    auto meta = Utf16Text(u"Name=A");
    meta.push_back(0x00);
    meta.push_back(0xD8);
    REQUIRE(ReadSaveMetaName(meta) == "A\xEF\xBF\xBD");
  }

  SECTION("Padding after a name is not part of it") {
    const auto meta = Utf16Text(std::u16string(u"Name=Foo") + u'\0' + u'\0');
    REQUIRE(ReadSaveMetaName(meta) == "Foo");
  }

  const auto packs = MakeContentMeta(
      Utf16Text(u"[Default]\r\nName=Bonus Map Pack\r\n[DE]\r\n"
                u"Name=Bonus-Karten-Paket\r\n[JA]\r\nName=\r\n"));

  SECTION("Content is named in the language asked for") {
    REQUIRE(ReadContentMetaName(packs, XLanguage::kGerman) ==
            "Bonus-Karten-Paket");
  }

  SECTION("A language without a name falls back to the default") {
    REQUIRE(ReadContentMetaName(packs, XLanguage::kJapanese) ==
            "Bonus Map Pack");
    REQUIRE(ReadContentMetaName(packs, XLanguage::kRussian) ==
            "Bonus Map Pack");
  }

  SECTION("Without a default name, English comes before other languages") {
    const auto meta = MakeContentMeta(
        Utf16Text(u"[JA]\r\nName=Japanese\r\n[EN]\r\nName=English\r\n"));
    REQUIRE(ReadContentMetaName(meta, XLanguage::kGerman) == "English");
  }

  SECTION("Quotes around a name are not part of it") {
    const auto meta = MakeContentMeta(
        Utf16Text(u"[default]\r\nName=\"Blastacular Pack\"\r\n"));
    REQUIRE(ReadContentMetaName(meta, XLanguage::kEnglish) ==
            "Blastacular Pack");
  }

  SECTION("A title update's binary section names nothing") {
    const auto meta = MakeContentMeta({1, 0, 0, 0, 0x46, 0, 0, 0});
    REQUIRE(ReadContentMetaName(meta, XLanguage::kEnglish).empty());
  }

  SECTION("Anything else names nothing") {
    auto meta = packs;
    meta[0x14] = 0;
    REQUIRE(ReadContentMetaName(meta, XLanguage::kEnglish).empty());
    meta = packs;
    Put<uint32_t>(meta, 0x34, uint32_t(meta.size()));
    REQUIRE(ReadContentMetaName(meta, XLanguage::kEnglish).empty());
  }
}

TEST_CASE("Original Xbox title data on disk", "[xbox_original]") {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-xbox-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto write = [](const std::filesystem::path& path,
                  const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  };
  const auto saves_dir = root / "UDATA" / "4D53004B";
  write(saves_dir / "AAAAAAAAAAAA" / "SaveMeta.xbx",
        Utf16Text(u"Name=career\r\n"));
  write(saves_dir / "CCCCCCCCCCCC" / "SaveMeta.xbx",
        Utf16Text(u"Name=Zeta\r\n"));
  write(saves_dir / "12E9194916CD" / "SaveMeta.xbx", {});
  std::filesystem::create_directories(saves_dir / "BBBBBBBBBBBB");
  write(saves_dir / "TitleMeta.xbx", Utf16Text(u"TitleName=PGR2\r\n"));
  const auto title_data = root / "TDATA" / "4d530064";
  write(title_data / "$c" / "4D53006400000001" / "ContentMeta.xbx",
        MakeContentMeta(Utf16Text(u"[Default]\r\nName=Bonus Map Pack\r\n")));
  std::filesystem::create_directories(title_data / "$c" / "4D53006400000002");
  write(title_data / "$u" / "default.xbe", {0});

  SECTION("Folders are found in any case") {
    REQUIRE(FindXboxOriginalPath(root / "UDATA", "4d53004b") == saves_dir);
    REQUIRE(FindXboxOriginalPath(root / "TDATA", "4D530064") == title_data);
    REQUIRE(FindXboxOriginalPath(title_data, "$U") == title_data / "$u");
    REQUIRE(FindXboxOriginalPath(root, "missing").empty());
  }

  SECTION("Only a save folder with a name is a save") {
    const auto saves = ListXboxOriginalSaves(saves_dir);
    REQUIRE(saves.size() == 2);
    REQUIRE(saves[0].name == "career");
    REQUIRE(saves[0].path.filename() == "AAAAAAAAAAAA");
    REQUIRE(saves[1].name == "Zeta");
  }

  SECTION("Each content folder is a package") {
    const auto content =
        ListXboxOriginalContent(title_data / "$c", XLanguage::kEnglish);
    REQUIRE(content.size() == 2);
    REQUIRE(content[0].name == "Bonus Map Pack");
    REQUIRE(content[1].name.empty());
  }

  std::filesystem::remove_all(root);
}

// A XEX2 header holding just the security info, which allows |media|.
static std::vector<uint8_t> MakeXex(uint32_t media,
                                    uint32_t header_size = 0x200) {
  std::vector<uint8_t> xex(0x200);
  Put<uint32_t>(xex, 0x0, xe::byte_swap(0x58455832u));
  Put<uint32_t>(xex, 0x8, xe::byte_swap(header_size));
  Put<uint32_t>(xex, 0x10, xe::byte_swap(0x20u));
  Put<uint32_t>(xex, 0x20 + 0x17C, xe::byte_swap(media));
  return xex;
}

TEST_CASE("XEX allowed media", "[xex]") {
  SECTION("An arcade title runs only from a package") {
    const auto xex = MakeXex(0x18000000);
    auto m = ExtractXexMetadata(xex.data(), xex.size());
    REQUIRE(m);
    REQUIRE(m->allowed_media_types == 0x18000000);
    REQUIRE(m->RunsFromPackageOnly());
  }

  SECTION("A disc title, or one converted to Games on Demand, does not") {
    for (uint32_t media : {0x00000004u, 0x00001000u}) {
      const auto xex = MakeXex(media);
      auto m = ExtractXexMetadata(xex.data(), xex.size());
      REQUIRE(m);
      REQUIRE(!m->RunsFromPackageOnly());
    }
  }

  SECTION("Security info past the header allows nothing") {
    const auto xex = MakeXex(0x18000000, 0x100);
    auto m = ExtractXexMetadata(xex.data(), xex.size());
    REQUIRE(m);
    REQUIRE(m->allowed_media_types == 0);
    REQUIRE(!m->RunsFromPackageOnly());
  }
}

TEST_CASE("Host path entry found by its new name after a rename",
          "[vfs_rename]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-rename-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "item.tmp") << "data";

  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* entry = device.ResolvePath("item.tmp");
    REQUIRE(entry != nullptr);

    entry->Rename("cache:\\item.ipk");

    REQUIRE(entry->name() == "item.ipk");
    REQUIRE(device.ResolvePath("item.ipk") == entry);
    REQUIRE(device.ResolvePath("item.tmp") == nullptr);
    REQUIRE(std::filesystem::exists(dir / "item.ipk"));
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("Host path entry moves into the directory it is renamed into",
          "[vfs_rename]") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("xenia-vfs-rename-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir / "pack");
  std::ofstream(dir / "temp.bin") << "data";

  {
    HostPathDevice device("\\Device\\Test", dir, false);
    REQUIRE(device.Initialize());
    Entry* entry = device.ResolvePath("temp.bin");
    REQUIRE(entry != nullptr);

    SECTION("into an existing directory") {
      entry->Rename("cache:\\pack\\item.arc");

      REQUIRE(device.ResolvePath("pack\\item.arc") == entry);
      REQUIRE(entry->parent() == device.ResolvePath("pack"));
      REQUIRE(entry->path() == "pack\\item.arc");
      REQUIRE(device.ResolvePath("item.arc") == nullptr);
      REQUIRE(device.ResolvePath("temp.bin") == nullptr);
      REQUIRE(std::filesystem::exists(dir / "pack" / "item.arc"));
    }

    SECTION("into a missing directory") {
      entry->Rename("cache:\\missing\\item.arc");

      REQUIRE(device.ResolvePath("temp.bin") == entry);
      REQUIRE(entry->name() == "temp.bin");
      REQUIRE(std::filesystem::exists(dir / "temp.bin"));
    }
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("Host path rename checks the target name", "[vfs_rename]") {
  struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("xenia-vfs-rename-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TemporaryDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } temporary;
  REQUIRE(std::filesystem::create_directories(temporary.path / "dir"));
  std::ofstream(temporary.path / "new.bin") << "new";
  std::ofstream(temporary.path / "old.bin") << "old";

  HostPathDevice device("test:", temporary.path, false);
  REQUIRE(device.Initialize());
  Entry* entry = device.ResolvePath("new.bin");
  Entry* old_entry = device.ResolvePath("old.bin");
  REQUIRE(entry);
  REQUIRE(old_entry);
  auto read = [&](const char* name) {
    std::string text;
    std::ifstream(temporary.path / name) >> text;
    return text;
  };

  SECTION("A missing directory is reported") {
    REQUIRE(entry->Rename("cache:\\missing\\item.bin") ==
            X_STATUS_OBJECT_PATH_NOT_FOUND);
  }

  SECTION("An existing file is kept without replace") {
    REQUIRE(entry->Rename("cache:\\OLD.BIN") == X_STATUS_OBJECT_NAME_COLLISION);
    REQUIRE(device.ResolvePath("old.bin") == old_entry);
    REQUIRE(device.ResolvePath("new.bin") == entry);
    REQUIRE(read("old.bin") == "old");
  }

  SECTION("An existing file is replaced with replace") {
    REQUIRE(entry->Rename("cache:\\old.bin", true) == X_STATUS_SUCCESS);
    REQUIRE(device.ResolvePath("old.bin") == entry);
    REQUIRE(device.ResolvePath("new.bin") == nullptr);
    REQUIRE(device.ResolvePath("")->child_count() == 2);
    REQUIRE(read("old.bin") == "new");
  }

  SECTION("An open file is not replaced") {
    File* file = nullptr;
    REQUIRE(old_entry->Open(FileAccess::kGenericRead, &file) ==
            X_STATUS_SUCCESS);
    REQUIRE(entry->Rename("cache:\\old.bin", true) == X_STATUS_ACCESS_DENIED);
    file->Destroy();
    REQUIRE(device.ResolvePath("old.bin") == old_entry);
    REQUIRE(read("old.bin") == "old");
  }

  SECTION("A directory is not replaced") {
    REQUIRE(entry->Rename("cache:\\dir", true) ==
            X_STATUS_OBJECT_NAME_COLLISION);
    REQUIRE(std::filesystem::is_directory(temporary.path / "dir"));
  }

  SECTION("Its own name is refused") {
    REQUIRE(entry->Rename("cache:\\NEW.BIN") == X_STATUS_OBJECT_NAME_COLLISION);
    REQUIRE(entry->Rename("cache:\\NEW.BIN", true) == X_STATUS_ACCESS_DENIED);
    REQUIRE(entry->name() == "new.bin");
  }
}

TEST_CASE("Host deletion waits for the last open handle", "[host_file]") {
  struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("xenia-vfs-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TemporaryDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } temporary;
  REQUIRE(std::filesystem::create_directories(temporary.path / "dir"));
  std::ofstream(temporary.path / "shared.tmp") << "abc";
  std::ofstream(temporary.path / "dir" / "inner.tmp") << "abc";

  HostPathDevice device("test:", temporary.path, false);
  REQUIRE(device.Initialize());
  auto close = [](File* file) {
    if (file) {
      file->Destroy();
    }
  };
  auto open = [&](Entry* entry) {
    File* file = nullptr;
    REQUIRE(entry->Open(FileAccess::kGenericRead, &file) == X_STATUS_SUCCESS);
    return std::unique_ptr<File, decltype(close)>(file, close);
  };

  SECTION("A file is deleted once its last handle closes") {
    auto* entry = device.ResolvePath("shared.tmp");
    REQUIRE(entry);
    auto first = open(entry);
    auto second = open(entry);
    entry->SetForDeletion(true);
    REQUIRE_FALSE(entry->Delete());
    first.reset();
    REQUIRE(std::filesystem::exists(temporary.path / "shared.tmp"));
    std::array<uint8_t, 3> bytes{};
    size_t read = 0;
    REQUIRE(second->ReadSync(bytes, 0, &read) == X_STATUS_SUCCESS);
    REQUIRE(read == bytes.size());
    REQUIRE(bytes == std::array<uint8_t, 3>{'a', 'b', 'c'});
    second.reset();
    REQUIRE_FALSE(std::filesystem::exists(temporary.path / "shared.tmp"));
    REQUIRE(device.ResolvePath("shared.tmp") == nullptr);
  }

  SECTION("A directory is kept while a file inside it is open") {
    auto* directory = device.ResolvePath("dir");
    REQUIRE(directory);
    auto* inner = directory->GetChild("inner.tmp");
    REQUIRE(inner);
    auto file = open(inner);
    REQUIRE_FALSE(directory->Delete());
    REQUIRE(std::filesystem::exists(temporary.path / "dir" / "inner.tmp"));
    file.reset();
    REQUIRE(directory->Delete());
    REQUIRE_FALSE(std::filesystem::exists(temporary.path / "dir"));
    REQUIRE(device.ResolvePath("dir") == nullptr);
  }
}

}  // namespace xe::vfs::test
