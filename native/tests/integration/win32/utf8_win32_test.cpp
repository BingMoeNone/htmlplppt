#include "test_main.h"

#ifdef _WIN32

#include <string>
#include <string_view>

#include "platform/win32/args_win32.h"
#include "platform/win32/utf8_win32.h"

using htmlppt::CommandLineUtf8Arguments;
using htmlppt::SplitCommandLineUtf8;
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

HTMLPPT_TEST_CASE(win32_argv_splits_a_utf8_command_line) {
  // The command line the editor builds for a CJK config path; CommandLineToArgvW
  // is the parser the child's CRT would use, so the split must preserve it.
  const auto args = SplitCommandLineUtf8(
      L"\"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe\" --config "
      L"\"D:\\\u6F14\u793A\\\u914D\u7F6E.json\"");
  HTMLPPT_CHECK(args.has_value());
  HTMLPPT_CHECK_EQ(args->size(), std::size_t{3});
  HTMLPPT_CHECK_EQ((*args)[0],
                   std::string("C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe"));
  HTMLPPT_CHECK_EQ((*args)[1], std::string("--config"));
  HTMLPPT_CHECK_EQ((*args)[2], std::string("D:\\") + "\xE6\xBC\x94\xE7\xA4\xBA" +
                                     "\\" + "\xE9\x85\x8D\xE7\xBD\xAE.json");
}

HTMLPPT_TEST_CASE(win32_argv_keeps_a_trailing_backslash_argument) {
  // The backslashes before the closing quote must collapse to one, not escape
  // the quote: this is the case the old QuoteArgument got wrong.
  const auto args = SplitCommandLineUtf8(L"\"exe\" --state-dir \"D:\\run\\\\\"");
  HTMLPPT_CHECK(args.has_value());
  HTMLPPT_CHECK_EQ(args->size(), std::size_t{3});
  HTMLPPT_CHECK_EQ((*args)[2], std::string("D:\\run\\"));
}

HTMLPPT_TEST_CASE(win32_argv_rejects_an_empty_command_line) {
  HTMLPPT_CHECK(!SplitCommandLineUtf8(std::wstring_view()).has_value());
}

HTMLPPT_TEST_CASE(win32_argv_reads_the_process_command_line) {
  const auto args = CommandLineUtf8Arguments();
  HTMLPPT_CHECK(args.has_value());
  HTMLPPT_CHECK(!args->empty());
  HTMLPPT_CHECK((*args)[0].find("htmlppt_win32_tests.exe") != std::string::npos);
}

#endif // _WIN32
