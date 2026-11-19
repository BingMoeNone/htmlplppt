#include "test_main.h"

#include <string>

#include "core/command_line.h"

using htmlppt::QuoteWindowsArgument;

HTMLPPT_TEST_CASE(command_line_quotes_a_plain_value) {
  HTMLPPT_CHECK_EQ(QuoteWindowsArgument(L"value"), std::wstring(L"\"value\""));
  HTMLPPT_CHECK_EQ(QuoteWindowsArgument(std::wstring()),
                   std::wstring(L"\"\""));
}

HTMLPPT_TEST_CASE(command_line_escapes_a_trailing_backslash) {
  // The closing quote must survive: CommandLineToArgvW would otherwise read the
  // backslash as escaping it and swallow the rest of the command line.
  HTMLPPT_CHECK_EQ(QuoteWindowsArgument(L"D:\\run\\"),
                   std::wstring(L"\"D:\\run\\\\\""));
}

HTMLPPT_TEST_CASE(command_line_escapes_an_embedded_quote) {
  HTMLPPT_CHECK_EQ(QuoteWindowsArgument(L"a\"b"),
                   std::wstring(L"\"a\\\"b\""));
}

HTMLPPT_TEST_CASE(command_line_doubles_backslashes_before_a_quote) {
  // 2n backslashes followed by a quote become 2n+1 backslashes and a literal
  // quote, i.e. the two backslashes are doubled before the escape.
  HTMLPPT_CHECK_EQ(QuoteWindowsArgument(L"a\\\\\"b"),
                   std::wstring(L"\"a\\\\\\\\\\\"b\""));
}
