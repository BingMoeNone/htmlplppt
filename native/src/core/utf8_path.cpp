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

} // namespace htmlppt
