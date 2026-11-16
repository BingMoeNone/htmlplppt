#include "test_main.h"

#include <filesystem>
#include <string>

#include "core/utf8_path.h"

using htmlppt::Utf8Path;

namespace {

std::u8string Utf8Bytes() {
  std::u8string value;
  value.push_back(static_cast<char8_t>(0xE6));
  value.push_back(static_cast<char8_t>(0xBC));
  value.push_back(static_cast<char8_t>(0x94));
  value.push_back(static_cast<char8_t>(0xE7));
  value.push_back(static_cast<char8_t>(0xA4));
  value.push_back(static_cast<char8_t>(0xBA));
  return value;
}

} // namespace

HTMLPPT_TEST_CASE(utf8_path_preserves_utf8_bytes) {
  const std::filesystem::path path(Utf8Bytes());
  const std::string expected = "\xE6\xBC\x94\xE7\xA4\xBA";
  HTMLPPT_CHECK_EQ(Utf8Path(path), expected);
}

HTMLPPT_TEST_CASE(utf8_path_preserves_ascii) {
  const std::filesystem::path path("/tmp/page1.html");
  HTMLPPT_CHECK_EQ(Utf8Path(path), std::string{"/tmp/page1.html"});
}
