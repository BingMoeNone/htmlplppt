#include "core/command_line.h"

namespace htmlppt {

std::wstring QuoteWindowsArgument(const std::wstring& value) {
  std::wstring quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back(L'"');
  // CommandLineToArgvW doubles a backslash only when it precedes a quote, and
  // reads 2n backslashes + a quote as n backslashes + a literal quote, 2n+1 as
  // n backslashes + an escaped quote. Count the run so both an embedded quote
  // and the closing quote stay unambiguous.
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  // The closing quote is preceded by the value's trailing backslashes, so they
  // are doubled: "C:\run\" must not turn its closing quote into an escape.
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

} // namespace htmlppt
