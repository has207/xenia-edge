/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/xbe_metadata.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <span>

#include "xenia/base/filesystem.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/xbox.h"

// Private to this file, as the vfs library doesn't link the copy the GPU
// library builds.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#undef _CRT_SECURE_NO_WARNINGS
#undef _CRT_NONSTDC_NO_DEPRECATE
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#include "third_party/stb/stb_image_write.h"
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace xe {
namespace vfs {

namespace {

constexpr uint32_t kXbeSignature = 0x48454258;  // "XBEH"
constexpr uint32_t kXprSignature = 0x30525058;  // "XPR0"

// XBE header fields, at file offsets. Addresses in it are where the loader
// maps the headers, from the base address.
constexpr size_t kXbeBaseAddress = 0x104;
constexpr size_t kXbeSizeOfHeaders = 0x108;
constexpr size_t kXbeCertificateAddress = 0x118;
constexpr size_t kXbeSectionCount = 0x11C;
constexpr size_t kXbeSectionHeadersAddress = 0x120;
constexpr size_t kXbeHeaderMinSize = 0x178;

constexpr size_t kSectionHeaderSize = 0x38;
constexpr size_t kSectionRawAddress = 0xC;
constexpr size_t kSectionRawSize = 0x10;
constexpr size_t kSectionNameAddress = 0x14;

constexpr size_t kCertificateTitleId = 0x8;
constexpr size_t kCertificateTitleName = 0xC;
constexpr size_t kCertificateTitleNameLength = 40;
constexpr size_t kCertificateDiscNumber = 0xA8;
constexpr size_t kCertificateVersion = 0xAC;
constexpr size_t kCertificateMinSize = 0xB0;

// Bounds on what a well-formed executable holds, so a damaged one can't make
// this read gigabytes.
constexpr size_t kMaxHeadersSize = 1024 * 1024;
constexpr size_t kMaxTitleImageSize = 4 * 1024 * 1024;

// The dashboard's title image section.
constexpr char kTitleImageSection[] = "$$XTIMAGE";

// Xbox Direct3D texture formats a title image comes in.
constexpr uint32_t kFormatA8R8G8B8 = 0x06;
constexpr uint32_t kFormatX8R8G8B8 = 0x07;
constexpr uint32_t kFormatDxt1 = 0x0C;
constexpr uint32_t kFormatDxt3 = 0x0E;
constexpr uint32_t kFormatDxt5 = 0x0F;
constexpr uint32_t kFormatLinearA8R8G8B8 = 0x12;
constexpr uint32_t kFormatLinearX8R8G8B8 = 0x1E;

// Reads |buffer.size()| bytes at |offset| of an executable. False past its end.
using XbeReader = std::function<bool(size_t offset, std::span<uint8_t> buffer)>;

uint32_t LoadU32(std::span<const uint8_t> data, size_t offset) {
  return xe::load<uint32_t>(data.data() + offset);
}

// Offset into a swizzled texture of a texel. Bits of x and y interleave, x
// first, until the smaller dimension runs out of them.
size_t SwizzledOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  size_t offset = 0;
  size_t bit = 1;
  for (uint32_t mask = 1; mask < width || mask < height; mask <<= 1) {
    if (mask < width) {
      if (x & mask) {
        offset |= bit;
      }
      bit <<= 1;
    }
    if (mask < height) {
      if (y & mask) {
        offset |= bit;
      }
      bit <<= 1;
    }
  }
  return offset;
}

void Rgb565ToRgba(uint16_t color, uint8_t* out) {
  const uint32_t r = (color >> 11) & 0x1F;
  const uint32_t g = (color >> 5) & 0x3F;
  const uint32_t b = color & 0x1F;
  out[0] = uint8_t((r << 3) | (r >> 2));
  out[1] = uint8_t((g << 2) | (g >> 4));
  out[2] = uint8_t((b << 3) | (b >> 2));
  out[3] = 255;
}

// Decodes a DXT color block into the 4x4 texels at |texels|, a row being
// |pitch| bytes. Three colors and transparent black only for DXT1.
void DecodeDxtColorBlock(const uint8_t* block, bool dxt1, uint8_t* texels,
                         size_t pitch) {
  const uint16_t c0 = xe::load<uint16_t>(block);
  const uint16_t c1 = xe::load<uint16_t>(block + 2);
  uint8_t palette[4][4];
  Rgb565ToRgba(c0, palette[0]);
  Rgb565ToRgba(c1, palette[1]);
  for (int i = 0; i < 3; ++i) {
    if (!dxt1 || c0 > c1) {
      palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i]) / 3);
    } else {
      palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = (!dxt1 || c0 > c1) ? 255 : 0;
  const uint32_t indices = xe::load<uint32_t>(block + 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const uint32_t index = (indices >> (2 * (y * 4 + x))) & 3;
      std::memcpy(texels + y * pitch + x * 4, palette[index], 4);
    }
  }
}

// Replaces the alpha of the 4x4 texels at |texels| from a DXT3 or DXT5 alpha
// block.
void DecodeDxtAlphaBlock(const uint8_t* block, bool dxt5, uint8_t* texels,
                         size_t pitch) {
  if (!dxt5) {
    const uint64_t alphas = xe::load<uint64_t>(block);
    for (int i = 0; i < 16; ++i) {
      const uint32_t alpha = (alphas >> (4 * i)) & 0xF;
      texels[(i / 4) * pitch + (i % 4) * 4 + 3] = uint8_t(alpha * 17);
    }
    return;
  }
  uint8_t palette[8];
  palette[0] = block[0];
  palette[1] = block[1];
  if (palette[0] > palette[1]) {
    for (int i = 1; i < 7; ++i) {
      palette[i + 1] = uint8_t(((7 - i) * palette[0] + i * palette[1]) / 7);
    }
  } else {
    for (int i = 1; i < 5; ++i) {
      palette[i + 1] = uint8_t(((5 - i) * palette[0] + i * palette[1]) / 5);
    }
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t indices = 0;
  for (int i = 0; i < 6; ++i) {
    indices |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (int i = 0; i < 16; ++i) {
    texels[(i / 4) * pitch + (i % 4) * 4 + 3] =
        palette[(indices >> (3 * i)) & 7];
  }
}

void AppendToVector(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

// Decodes an XPR0 texture bundle's first texture to PNG. Empty for a format
// it doesn't know.
std::vector<uint8_t> DecodeTitleImage(std::span<const uint8_t> xpr) {
  constexpr size_t kXprHeaderSize = 12;
  constexpr size_t kResourceData = kXprHeaderSize + 4;
  constexpr size_t kResourceFormat = kXprHeaderSize + 12;
  if (xpr.size() < kResourceFormat + 4 || LoadU32(xpr, 0) != kXprSignature) {
    return {};
  }
  const size_t data_start =
      size_t(LoadU32(xpr, 8)) + size_t(LoadU32(xpr, kResourceData));
  const uint32_t format = LoadU32(xpr, kResourceFormat);
  const uint32_t texel_format = (format >> 8) & 0xFF;
  const uint32_t width = 1u << ((format >> 20) & 0xF);
  const uint32_t height = 1u << ((format >> 24) & 0xF);
  if (width > 1024 || height > 1024 || data_start > xpr.size()) {
    return {};
  }
  const std::span<const uint8_t> data = xpr.subspan(data_start);
  const size_t pitch = size_t(width) * 4;
  std::vector<uint8_t> rgba(pitch * height);

  switch (texel_format) {
    case kFormatA8R8G8B8:
    case kFormatX8R8G8B8:
    case kFormatLinearA8R8G8B8:
    case kFormatLinearX8R8G8B8: {
      if (data.size() < rgba.size()) {
        return {};
      }
      const bool swizzled =
          texel_format == kFormatA8R8G8B8 || texel_format == kFormatX8R8G8B8;
      const bool opaque = texel_format == kFormatX8R8G8B8 ||
                          texel_format == kFormatLinearX8R8G8B8;
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const size_t texel = swizzled ? SwizzledOffset(x, y, width, height)
                                        : size_t(y) * width + x;
          const uint8_t* bgra = data.data() + texel * 4;
          uint8_t* out = rgba.data() + y * pitch + size_t(x) * 4;
          out[0] = bgra[2];
          out[1] = bgra[1];
          out[2] = bgra[0];
          out[3] = opaque ? 255 : bgra[3];
        }
      }
    } break;
    case kFormatDxt1:
    case kFormatDxt3:
    case kFormatDxt5: {
      if (width < 4 || height < 4) {
        return {};
      }
      const bool dxt1 = texel_format == kFormatDxt1;
      const size_t block_size = dxt1 ? 8 : 16;
      if (data.size() < size_t(width / 4) * (height / 4) * block_size) {
        return {};
      }
      const uint8_t* block = data.data();
      for (uint32_t y = 0; y < height; y += 4) {
        for (uint32_t x = 0; x < width; x += 4, block += block_size) {
          uint8_t* texels = rgba.data() + y * pitch + size_t(x) * 4;
          DecodeDxtColorBlock(dxt1 ? block : block + 8, dxt1, texels, pitch);
          if (!dxt1) {
            DecodeDxtAlphaBlock(block, texel_format == kFormatDxt5, texels,
                                pitch);
          }
        }
      }
    } break;
    default:
      return {};
  }

  std::vector<uint8_t> png;
  if (!stbi_write_png_to_func(AppendToVector, &png, int(width), int(height), 4,
                              rgba.data(), int(pitch))) {
    return {};
  }
  return png;
}

std::optional<XbeMetadata> ParseXbe(const XbeReader& read) {
  std::vector<uint8_t> headers(kXbeHeaderMinSize);
  if (!read(0, headers) || LoadU32(headers, 0) != kXbeSignature) {
    return std::nullopt;
  }
  const uint32_t base = LoadU32(headers, kXbeBaseAddress);
  const size_t headers_size = LoadU32(headers, kXbeSizeOfHeaders);
  if (headers_size < kXbeHeaderMinSize || headers_size > kMaxHeadersSize) {
    return std::nullopt;
  }
  headers.resize(headers_size);
  if (!read(0, headers)) {
    return std::nullopt;
  }
  // Header addresses are where the loader maps the headers.
  auto header_offset = [&](uint32_t address,
                           size_t size) -> std::optional<size_t> {
    if (address < base || address - base > headers_size ||
        headers_size - (address - base) < size) {
      return std::nullopt;
    }
    return size_t(address - base);
  };

  const auto certificate = header_offset(
      LoadU32(headers, kXbeCertificateAddress), kCertificateMinSize);
  if (!certificate) {
    return std::nullopt;
  }
  XbeMetadata metadata;
  metadata.title_id = LoadU32(headers, *certificate + kCertificateTitleId);
  metadata.disc_number =
      LoadU32(headers, *certificate + kCertificateDiscNumber);
  metadata.version = LoadU32(headers, *certificate + kCertificateVersion);
  std::u16string name;
  for (size_t i = 0; i < kCertificateTitleNameLength; ++i) {
    const char16_t c = xe::load<char16_t>(headers.data() + *certificate +
                                          kCertificateTitleName + i * 2);
    if (!c) {
      break;
    }
    name.push_back(c);
  }
  metadata.title_name = xe::to_utf8_lossy(name);

  const uint32_t section_count = LoadU32(headers, kXbeSectionCount);
  const auto sections =
      header_offset(LoadU32(headers, kXbeSectionHeadersAddress),
                    size_t(section_count) * kSectionHeaderSize);
  if (!sections) {
    return metadata;
  }
  for (uint32_t i = 0; i < section_count; ++i) {
    const size_t section = *sections + size_t(i) * kSectionHeaderSize;
    const auto name_offset =
        header_offset(LoadU32(headers, section + kSectionNameAddress),
                      sizeof(kTitleImageSection));
    if (!name_offset ||
        std::memcmp(headers.data() + *name_offset, kTitleImageSection,
                    sizeof(kTitleImageSection)) != 0) {
      continue;
    }
    const size_t image_size = LoadU32(headers, section + kSectionRawSize);
    if (image_size > kMaxTitleImageSize) {
      break;
    }
    std::vector<uint8_t> image(image_size);
    if (read(LoadU32(headers, section + kSectionRawAddress), image)) {
      metadata.icon_png = DecodeTitleImage(image);
    }
    break;
  }
  return metadata;
}

std::optional<XbeMetadata> ExtractFromPackage(
    const std::filesystem::path& path) {
  // The header comes back for any file, so its signature has to be checked.
  auto header = XContentContainerDevice::ReadContainerHeader(path);
  if (!header || !header->content_header.is_magic_valid() ||
      header->content_metadata.content_type.get() != XContentType::kXboxTitle) {
    return std::nullopt;
  }
  auto device = XContentContainerDevice::CreateContentDevice("", path);
  if (!device || !device->Initialize()) {
    return std::nullopt;
  }
  // Paths resolve through Device, as the file system does it.
  Entry* entry = static_cast<Device&>(*device).ResolvePath("default.xbe");
  File* file = nullptr;
  if (!entry || entry->Open(xe::filesystem::FileAccess::kFileReadData, &file) !=
                    X_STATUS_SUCCESS) {
    return std::nullopt;
  }
  auto metadata = ParseXbe([file](size_t offset, std::span<uint8_t> buffer) {
    size_t bytes_read = 0;
    return file->ReadSync(buffer, offset, &bytes_read) == X_STATUS_SUCCESS &&
           bytes_read == buffer.size();
  });
  file->Destroy();
  // The package's own thumbnail stands in for a missing title image.
  if (metadata && metadata->icon_png.empty()) {
    if (auto package = ExtractStfsMetadata(path)) {
      metadata->icon_png = std::move(package->icon_data);
    }
  }
  return metadata;
}

}  // namespace

std::optional<GdfxFileLocation> FindXboxOriginalXbe(const uint8_t* data,
                                                    size_t size) {
  auto partition = GdfxFindPartition(data, size);
  if (!partition || GdfxFindFile(data, size, *partition, "default.xex")) {
    return std::nullopt;
  }
  auto location = GdfxFindFile(data, size, *partition, "default.xbe");
  if (!location || location->offset > size ||
      size - location->offset < location->length) {
    return std::nullopt;
  }
  return location;
}

std::optional<XbeMetadata> ExtractXbeMetadata(const uint8_t* data,
                                              size_t size) {
  return ParseXbe([data, size](size_t offset, std::span<uint8_t> buffer) {
    if (offset > size || size - offset < buffer.size()) {
      return false;
    }
    std::memcpy(buffer.data(), data + offset, buffer.size());
    return true;
  });
}

std::optional<XbeMetadata> ExtractXbeMetadata(
    const std::filesystem::path& path) {
  uint32_t magic = 0;
  {
    auto file = xe::filesystem::FileHandle::OpenExisting(
        path, xe::filesystem::FileAccess::kGenericRead);
    size_t bytes_read = 0;
    if (!file || !file->Read(0, &magic, sizeof(magic), &bytes_read) ||
        bytes_read != sizeof(magic)) {
      return std::nullopt;
    }
    if (magic == kXbeSignature) {
      auto* handle = file.get();
      return ParseXbe([handle](size_t offset, std::span<uint8_t> buffer) {
        size_t read = 0;
        return handle->Read(offset, buffer.data(), buffer.size(), &read) &&
               read == buffer.size();
      });
    }
  }
  if (auto metadata = ExtractFromPackage(path)) {
    return metadata;
  }
  auto mmap = xe::MappedMemory::Open(path, xe::MappedMemory::Mode::kRead);
  if (!mmap) {
    return std::nullopt;
  }
  auto location = FindXboxOriginalXbe(mmap->data(), mmap->size());
  if (!location) {
    return std::nullopt;
  }
  return ExtractXbeMetadata(mmap->data() + location->offset, location->length);
}

}  // namespace vfs
}  // namespace xe
