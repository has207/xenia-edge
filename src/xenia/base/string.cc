/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/string.h"

#include <string.h>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

#if !XE_PLATFORM_WIN32
#include <strings.h>
#endif  // !XE_PLATFORM_WIN32

// https://github.com/nemtrif/utfcpp/issues/85
#if defined(_MSVC_LANG) && _MSVC_LANG > __cplusplus
#define UTF_CPP_CPLUSPLUS _MSVC_LANG
#endif
#include "third_party/utfcpp/source/utf8.h"

namespace utfcpp = utf8;

namespace xe {

int xe_strcasecmp(const char* string1, const char* string2) {
#if XE_PLATFORM_WIN32
  return _stricmp(string1, string2);
#else
  return strcasecmp(string1, string2);
#endif  // XE_PLATFORM_WIN32
}

int xe_strncasecmp(const char* string1, const char* string2, size_t count) {
#if XE_PLATFORM_WIN32
  return _strnicmp(string1, string2, count);
#else
  return strncasecmp(string1, string2, count);
#endif  // XE_PLATFORM_WIN32
}

char* xe_strdup(const char* source) {
#if XE_PLATFORM_WIN32
  return _strdup(source);
#else
  return strdup(source);
#endif  // XE_PLATFORM_WIN32
}

std::string to_utf8(const std::u16string_view source) {
  return utfcpp::utf16to8(source);
}

std::string to_utf8_lossy(const std::u16string_view source) {
  std::u16string text(source);
  for (size_t i = 0; i < text.size(); ++i) {
    const bool high = text[i] >= 0xD800 && text[i] <= 0xDBFF;
    const bool low = text[i] >= 0xDC00 && text[i] <= 0xDFFF;
    if (high && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
        text[i + 1] <= 0xDFFF) {
      ++i;
    } else if (high || low) {
      text[i] = char16_t(0xFFFD);
    }
  }
  return utfcpp::utf16to8(text);
}

std::u16string to_utf16(const std::string_view source) {
  return utfcpp::utf8to16(source);
}

namespace {
// Windows-1252 bytes 0x80-0x9F. The five it leaves undefined map to the C1
// control of the same value, as Windows maps them.
constexpr char32_t kWin1252C1[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};
}  // namespace

std::string utf8_to_win1252(const std::string_view source) {
  const std::string valid = utfcpp::replace_invalid(source);
  std::string result;
  result.reserve(valid.size());
  for (auto it = valid.cbegin(); it != valid.cend();) {
    const char32_t cp = utfcpp::unchecked::next(it);
    char byte = '_';
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
      byte = char(cp);
    } else {
      for (size_t i = 0; i < std::size(kWin1252C1); ++i) {
        if (kWin1252C1[i] == cp) {
          byte = char(0x80 + i);
          break;
        }
      }
    }
    result.push_back(byte);
  }
  return result;
}

std::string win1252_to_utf8(const std::string_view source) {
  std::string result;
  result.reserve(source.size());
  for (const char c : source) {
    const uint8_t byte = uint8_t(c);
    const char32_t cp =
        byte >= 0x80 && byte < 0xA0 ? kWin1252C1[byte - 0x80] : byte;
    utfcpp::append(cp, result);
  }
  return result;
}

}  // namespace xe
