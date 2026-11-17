#pragma once

#ifdef _WIN32

#include <optional>
#include <string>
#include <string_view>

namespace htmlppt {

// Converts UTF-8 bytes to UTF-16. Returns nullopt on invalid input or size
// overflow. An empty input produces an empty string.
std::optional<std::wstring> Utf8ToWide(std::string_view text);

// Converts UTF-16 to UTF-8. Returns nullopt on invalid input or size
// overflow. An empty input produces an empty string.
std::optional<std::string> WideToUtf8(std::wstring_view text);

} // namespace htmlppt

#endif // _WIN32
