/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/png_utils.h"

#include <algorithm>

#include "xenia/base/filesystem.h"

#include "third_party/stb/stb_image.h"

// Private to this file, as the base library doesn't link the copy the GPU
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

bool IsFilePngImage(const std::filesystem::path& file_path) {
  FILE* file = xe::filesystem::OpenFile(file_path, "rb");
  if (!file) {
    return false;
  }

  constexpr uint8_t magic_size = 4;
  char magic[magic_size];
  if (fread(&magic, 1, magic_size, file) != magic_size) {
    return false;
  }

  fclose(file);

  if (magic[1] != 'P' || magic[2] != 'N' || magic[3] != 'G') {
    return false;
  }

  return true;
}

std::pair<uint16_t, uint16_t> GetImageResolution(
    const std::filesystem::path& file_path) {
  FILE* file = xe::filesystem::OpenFile(file_path, "rb");
  if (!file) {
    return {};
  }

  int width, height, channels;
  if (!stbi_info_from_file(file, &width, &height, &channels)) {
    return {};
  }

  fclose(file);
  return {width, height};
}

std::vector<uint8_t> ReadPngFromFile(const std::filesystem::path& file_path) {
  FILE* file = xe::filesystem::OpenFile(file_path, "rb");
  if (!file) {
    return {};
  }

  const auto file_size = std::filesystem::file_size(file_path);
  std::vector<uint8_t> data(file_size);
  fread(data.data(), 1, file_size, file);
  fclose(file);

  return data;
}

std::vector<uint8_t> ResizePng(std::span<const uint8_t> image, uint32_t width,
                               uint32_t height) {
  int src_width, src_height, channels;
  uint8_t* src =
      stbi_load_from_memory(image.data(), static_cast<int>(image.size()),
                            &src_width, &src_height, &channels, 4);
  if (!src) {
    return {};
  }

  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t y0 = y * src_height / height;
    const uint32_t y1 = std::max(y0 + 1, (y + 1) * src_height / height);
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t x0 = x * src_width / width;
      const uint32_t x1 = std::max(x0 + 1, (x + 1) * src_width / width);
      // Colors weighted by alpha so transparent pixels don't darken edges.
      uint32_t color[3] = {};
      uint32_t alpha = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        for (uint32_t sx = x0; sx < x1; ++sx) {
          const uint8_t* pixel = src + (size_t(sy) * src_width + sx) * 4;
          for (int c = 0; c < 3; ++c) {
            color[c] += pixel[c] * pixel[3];
          }
          alpha += pixel[3];
        }
      }
      uint8_t* out = rgba.data() + (size_t(y) * width + x) * 4;
      for (int c = 0; c < 3; ++c) {
        out[c] = alpha ? uint8_t(color[c] / alpha) : 0;
      }
      out[3] = uint8_t(alpha / ((y1 - y0) * (x1 - x0)));
    }
  }
  stbi_image_free(src);

  std::vector<uint8_t> png;
  auto append = [](void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
  };
  if (!stbi_write_png_to_func(append, &png, int(width), int(height), 4,
                              rgba.data(), int(width * 4))) {
    return {};
  }
  return png;
}

}  // namespace xe
