#pragma once

#ifdef _WIN32

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/window_system.h"

namespace htmlppt {

std::vector<std::uint32_t> ProcessIdsByName(std::wstring_view image_name);

// Returns the full command line for a process when it can be queried.
std::optional<std::wstring> ProcessCommandLine(std::uint32_t pid);

// Returns processes whose executable name matches and whose command line
// contains the marker. Markers are compared case-sensitively.
std::vector<std::uint32_t> ProcessIdsByCommandLineMarker(
    std::wstring_view image_name, std::wstring_view marker);

// Finds the largest visible window owned by a process whose command line
// contains the marker. Used for per-profile Edge window matching.
std::optional<WindowId> FindWindowForProcessMarker(
    WindowSystem& windows, std::wstring_view image_name,
    std::wstring_view marker);

} // namespace htmlppt

#endif // _WIN32
