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

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/mstream.h>

namespace xe {
namespace ui {

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
  if (image.GetWidth() != size_px || image.GetHeight() != size_px) {
    image.Rescale(size_px, size_px, wxIMAGE_QUALITY_HIGH);
  }
  wxBitmap bmp(image);
  bmp.SetScaleFactor(scale);
  return wxBitmapBundle::FromBitmap(bmp);
}

wxBitmapBundle WrapRgbaIcon(const std::vector<uint8_t>& rgba, int width,
                            int height, int size_px, double scale) {
  if (rgba.empty() || width <= 0 || height <= 0 ||
      rgba.size() != static_cast<size_t>(width) * height * 4) {
    return wxBitmapBundle();
  }
  const size_t pixel_count = static_cast<size_t>(width) * height;
  auto* rgb = static_cast<unsigned char*>(std::malloc(pixel_count * 3));
  auto* alpha = static_cast<unsigned char*>(std::malloc(pixel_count));
  if (!rgb || !alpha) {
    std::free(rgb);
    std::free(alpha);
    return wxBitmapBundle();
  }
  for (size_t i = 0; i < pixel_count; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    alpha[i] = rgba[i * 4 + 3];
  }
  wxImage image(width, height, rgb, alpha);
  if (image.GetWidth() != size_px || image.GetHeight() != size_px) {
    image.Rescale(size_px, size_px, wxIMAGE_QUALITY_HIGH);
  }
  wxBitmap bmp(image);
  bmp.SetScaleFactor(scale);
  return wxBitmapBundle::FromBitmap(bmp);
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
