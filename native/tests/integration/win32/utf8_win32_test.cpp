#include "test_main.h"

#ifdef _WIN32

#include <string>
#include <string_view>

#include "platform/win32/utf8_win32.h"

using htmlppt::Utf8ToWide;
using htmlppt::WideToUtf8;

HTMLPPT_TEST_CASE(win32_utf8_round_trips_ascii) {
  const auto wide = Utf8ToWide("C:/demo/page1.html");
  HTMLPPT_CHECK(wide.has_value());
  HTMLPPT_CHECK_EQ(*wide, std::wstring(L"C:/demo/page1.html"));
  const auto narrow = WideToUtf8(*wide);
  HTMLPPT_CHECK(narrow.has_value());
  HTMLPPT_CHECK_EQ(*narrow, std::string("C:/demo/page1.html"));
}

HTMLPPT_TEST_CASE(win32_utf8_round_trips_cjk_and_emoji) {
  // UTF-8 for two CJK characters followed by U+1F600.
  const std::string text =
      "\xE6\xBC\x94\xE7\xA4\xBA\xF0\x9F\x98\x80";
  const auto wide = Utf8ToWide(text);
  HTMLPPT_CHECK(wide.has_value());
  HTMLPPT_CHECK_EQ(wide->size(), std::size_t{4});
  const auto narrow = WideToUtf8(*wide);
  HTMLPPT_CHECK(narrow.has_value());
  HTMLPPT_CHECK_EQ(*narrow, text);
}

HTMLPPT_TEST_CASE(win32_utf8_rejects_invalid_utf8) {
  const std::string invalid("\xC3\x28", 2);
  HTMLPPT_CHECK(!Utf8ToWide(invalid).has_value());
}

HTMLPPT_TEST_CASE(win32_utf8_rejects_invalid_utf16) {
  const std::wstring invalid(1, static_cast<wchar_t>(0xD800));
  HTMLPPT_CHECK(!WideToUtf8(invalid).has_value());
}

HTMLPPT_TEST_CASE(win32_utf8_handles_empty_input) {
  const auto wide = Utf8ToWide(std::string_view());
  HTMLPPT_CHECK(wide.has_value());
  HTMLPPT_CHECK(wide->empty());
  const auto narrow = WideToUtf8(std::wstring_view());
  HTMLPPT_CHECK(narrow.has_value());
  HTMLPPT_CHECK(narrow->empty());
}

#endif // _WIN32
