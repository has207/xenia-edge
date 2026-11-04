/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/icon_decode.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include "third_party/stb/stb_image.h"

namespace xe {
namespace ui {

namespace {

// Splits row-major 8-bit RGBA into the malloc'd RGB and alpha buffers wxImage
// takes ownership of.
// Returns an invalid image when they can't be allocated.
wxImage RgbaToImage(const uint8_t* rgba, int width, int height) {
  const size_t pixel_count = static_cast<size_t>(width) * height;
  auto* rgb = static_cast<unsigned char*>(std::malloc(pixel_count * 3));
  auto* alpha = static_cast<unsigned char*>(std::malloc(pixel_count));
  if (!rgb || !alpha) {
    std::free(rgb);
    std::free(alpha);
    return wxImage();
  }
  for (size_t i = 0; i < pixel_count; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    alpha[i] = rgba[i * 4 + 3];
  }
  return wxImage(width, height, rgb, alpha);
}

wxBitmapBundle ScaledBundle(wxImage image, const wxSize& size, double scale) {
  if (!image.IsOk()) {
    return wxBitmapBundle();
  }
  if (image.GetSize() != size) {
    image.Rescale(size.x, size.y, wxIMAGE_QUALITY_HIGH);
  }
  wxBitmap bmp(image);
  bmp.SetScaleFactor(scale);
  return wxBitmapBundle::FromBitmap(bmp);
}

}  // namespace

wxBitmapBundle DecodePngIcon(const std::vector<uint8_t>& png, int size_px,
                             double scale) {
  if (png.empty()) {
    return wxBitmapBundle();
  }
  wxMemoryInputStream stream(png.data(), png.size());
  wxImage image;
  if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
    return wxBitmapBundle();
  }
  return ScaledBundle(std::move(image), wxSize(size_px, size_px), scale);
}

wxBitmapBundle WrapRgbaIcon(const std::vector<uint8_t>& rgba, int width,
                            int height, int size_px, double scale) {
  if (rgba.empty() || width <= 0 || height <= 0 ||
      rgba.size() != static_cast<size_t>(width) * height * 4) {
    return wxBitmapBundle();
  }
  return ScaledBundle(RgbaToImage(rgba.data(), width, height),
                      wxSize(size_px, size_px), scale);
}

wxBitmapBundle DecodeCoverArt(const std::vector<uint8_t>& data,
                              const wxSize& size, double scale) {
  if (data.empty()) {
    return wxBitmapBundle();
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  // Covers are a few hundred pixels across, so a bigger one is corrupt.
  constexpr int kMaxSide = 4096;
  if (!stbi_info_from_memory(data.data(), int(data.size()), &width, &height,
                             &channels) ||
      width > kMaxSide || height > kMaxSide) {
    return wxBitmapBundle();
  }
  stbi_uc* rgba = stbi_load_from_memory(data.data(), int(data.size()), &width,
                                        &height, &channels, 4);
  if (!rgba) {
    return wxBitmapBundle();
  }
  wxImage image = RgbaToImage(rgba, width, height);
  stbi_image_free(rgba);
  return ScaledBundle(std::move(image), size, scale);
}

wxBitmapBundle MakeTextPlaceholder(const wxString& text, int size_px,
                                   double scale) {
  wxBitmap bmp(size_px, size_px, 32);
  wxMemoryDC dc(bmp);
  dc.SetBackground(wxBrush(wxColour(60, 60, 60)));
  dc.Clear();
  wxFont font = dc.GetFont();
  font.Scale(0.85f * static_cast<float>(scale));
  dc.SetFont(font);
  dc.SetTextForeground(wxColour(180, 180, 180));
  wxString line1 = text.BeforeFirst('\n');
  wxString line2 = text.AfterFirst('\n');
  wxSize l1 = dc.GetTextExtent(line1);
  const int gap = std::max(1, static_cast<int>(2 * scale));
  if (line2.empty()) {
    int y = (size_px - l1.y) / 2;
    dc.DrawText(line1, (size_px - l1.x) / 2, y);
  } else {
    wxSize l2 = dc.GetTextExtent(line2);
    int total_h = l1.y + l2.y + gap;
    int y = (size_px - total_h) / 2;
    dc.DrawText(line1, (size_px - l1.x) / 2, y);
    dc.DrawText(line2, (size_px - l2.x) / 2, y + l1.y + gap);
  }
  dc.SelectObject(wxNullBitmap);
  bmp.SetScaleFactor(scale);
  return wxBitmapBundle::FromBitmap(bmp);
}

}  // namespace ui
}  // namespace xe
