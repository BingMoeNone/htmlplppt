#include "core/utf8_path.h"

#include <cwctype>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace htmlppt {
namespace {

std::filesystem::path Normalized(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) absolute = path;
  return absolute.lexically_normal();
}

bool ComponentEquals(const std::filesystem::path& left,
                     const std::filesystem::path& right) {
#ifdef _WIN32
  const std::wstring left_text = left.wstring();
  const std::wstring right_text = right.wstring();
  if (left_text.size() != right_text.size()) return false;
  for (std::size_t i = 0; i < left_text.size(); ++i) {
    if (std::towlower(static_cast<wint_t>(left_text[i])) !=
        std::towlower(static_cast<wint_t>(right_text[i]))) {
      return false;
    }
  }
  return true;
#else
  return left == right;
#endif
}

} // namespace

std::string Utf8Path(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  std::string result;
  result.reserve(utf8.size());
  for (const char8_t value : utf8) {
    result.push_back(static_cast<char>(value));
  }
  return result;
}

std::filesystem::path PathFromUtf8(std::string_view text) {
  std::u8string utf8;
  utf8.reserve(text.size());
  for (const unsigned char value : text) {
    utf8.push_back(static_cast<char8_t>(value));
  }
  return std::filesystem::path(utf8);
}

std::filesystem::path ResolveConfigPath(const std::filesystem::path& config_path,
                                        const std::filesystem::path& current_dir) {
  if (config_path.empty()) return {};
  std::filesystem::path resolved = config_path;
  if (resolved.is_relative() && !current_dir.empty()) {
    resolved = current_dir / resolved;
  }
  return resolved;
}

std::filesystem::path ConfigDirFor(const std::filesystem::path& config_path,
                                   const std::filesystem::path& current_dir) {
  return ResolveConfigPath(config_path, current_dir).parent_path();
}

bool PathEquals(const std::filesystem::path& left,
                const std::filesystem::path& right) {
  const std::filesystem::path left_norm = Normalized(left);
  const std::filesystem::path right_norm = Normalized(right);
  auto left_it = left_norm.begin();
  auto right_it = right_norm.begin();
  for (; left_it != left_norm.end() && right_it != right_norm.end();
       ++left_it, ++right_it) {
    if (!ComponentEquals(*left_it, *right_it)) return false;
  }
  return left_it == left_norm.end() && right_it == right_norm.end();
}

bool PathIsInside(const std::filesystem::path& child,
                  const std::filesystem::path& parent) {
  const std::filesystem::path child_norm = Normalized(child);
  const std::filesystem::path parent_norm = Normalized(parent);
  auto child_it = child_norm.begin();
  for (auto parent_it = parent_norm.begin(); parent_it != parent_norm.end();
       ++parent_it, ++child_it) {
    if (child_it == child_norm.end() ||
        !ComponentEquals(*child_it, *parent_it)) {
      return false;
    }
  }
  return child_it != child_norm.end();
}

bool WriteFileAtomically(const std::filesystem::path& path,
                         std::string_view text, std::string* error) {
  const auto fail = [error](std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  };
  if (path.empty()) return fail("no path to write");
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      return fail("failed to open " + Utf8Path(temp) + " for writing");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) {
      std::error_code cleanup;
      std::filesystem::remove(temp, cleanup);
      return fail("failed to write " + Utf8Path(temp));
    }
  }
#ifdef _WIN32
  if (MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD last_error = GetLastError();
    std::error_code cleanup;
    std::filesystem::remove(temp, cleanup);
    return fail("failed to replace " + Utf8Path(path) +
                " (Windows error " + std::to_string(last_error) + ")");
  }
#else
  std::error_code rename_error;
  std::filesystem::rename(temp, path, rename_error);
  if (rename_error) {
    std::error_code cleanup;
    std::filesystem::remove(temp, cleanup);
    return fail("failed to replace " + Utf8Path(path) + ": " +
                rename_error.message());
  }
#endif
  return true;
}

} // namespace htmlppt
