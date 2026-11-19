#pragma once

#include <string>

namespace htmlppt {

// Encodes one argument for a Windows command line, following the rules the CRT
// (and CommandLineToArgvW) apply when it splits argv: the value is always
// quoted, embedded quotes are escaped with a backslash, and the run of
// backslashes immediately before a quote (including the closing one) is doubled
// so the quote is not swallowed. Always quoting keeps a value that needs no
// quotes parse-equivalent, and makes the output deterministic.
std::wstring QuoteWindowsArgument(const std::wstring& value);

} // namespace htmlppt
