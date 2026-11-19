#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace htmlppt {

#ifdef _WIN32
// Splits a Windows command line into UTF-8 arguments with CommandLineToArgvW,
// the parser the CRT uses for a child's argv. The Windows CRT itself builds
// argv in the ANSI code page, which mangles every character the code page
// cannot represent (CJK paths, emoji); parsing the UTF-16 command line once and
// keeping the arguments UTF-8 avoids that second conversion entirely.
// Returns nullopt for an empty command line or an argument that cannot be
// represented in UTF-8.
std::optional<std::vector<std::string>> SplitCommandLineUtf8(
    std::wstring_view command_line);

// This process's GetCommandLineW, split into UTF-8 arguments.
std::optional<std::vector<std::string>> CommandLineUtf8Arguments();
#endif

} // namespace htmlppt
