#include "core/utf8_path.h"

namespace htmlppt {

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

std::filesystem::path ConfigDirFor(const std::filesystem::path& config_path,
                                   const std::filesystem::path& current_dir) {
  if (config_path.empty()) return {};
  std::filesystem::path resolved = config_path;
  if (resolved.is_relative() && !current_dir.empty()) {
    resolved = current_dir / resolved;
  }
  return resolved.parent_path();
}

} // namespace htmlppt
