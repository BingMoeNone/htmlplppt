#include "platform/win32/args_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <shellapi.h>

#include <utility>

#include "platform/win32/utf8_win32.h"

namespace htmlppt {
namespace {

// CommandLineToArgvW takes a mutable pointer, so every call needs its own copy
// of the line. The returned array and its strings are LocalFree'd by the caller.
struct LocalArgv {
  LPWSTR* values = nullptr;
  int count = 0;

  LocalArgv(const wchar_t* line, int* count_out) {
    values = CommandLineToArgvW(line, &count);
    if (count_out != nullptr) *count_out = count;
  }
  ~LocalArgv() {
    if (values != nullptr) LocalFree(values);
  }
  LocalArgv(const LocalArgv&) = delete;
  LocalArgv& operator=(const LocalArgv&) = delete;
};

} // namespace

std::optional<std::vector<std::string>> SplitCommandLineUtf8(
    std::wstring_view command_line) {
  if (command_line.empty()) return std::nullopt;
  std::wstring mutable_line(command_line);
  int count = 0;
  LocalArgv parsed(mutable_line.c_str(), &count);
  if (parsed.values == nullptr || count <= 0) return std::nullopt;
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    auto utf8 = WideToUtf8(parsed.values[index]);
    if (!utf8.has_value()) return std::nullopt;
    arguments.push_back(std::move(*utf8));
  }
  return arguments;
}

std::optional<std::vector<std::string>> CommandLineUtf8Arguments() {
  const wchar_t* line = GetCommandLineW();
  if (line == nullptr) return std::nullopt;
  return SplitCommandLineUtf8(line);
}

} // namespace htmlppt

#endif // _WIN32
