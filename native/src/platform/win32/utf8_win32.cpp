#include "platform/win32/utf8_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <climits>

namespace htmlppt {

std::optional<std::wstring> Utf8ToWide(std::string_view text) {
  if (text.empty()) return std::wstring();
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int length = static_cast<int>(text.size());
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       text.data(), length, nullptr, 0);
  if (size <= 0) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  const int written = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size);
  if (written != size) return std::nullopt;
  return result;
}

std::optional<std::string> WideToUtf8(std::wstring_view text) {
  if (text.empty()) return std::string();
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       text.data(), length, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return std::nullopt;
  std::string result(static_cast<std::size_t>(size), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size,
      nullptr, nullptr);
  if (written != size) return std::nullopt;
  return result;
}

} // namespace htmlppt

#endif // _WIN32
