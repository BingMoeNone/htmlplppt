#pragma once

#ifdef _WIN32

#include <string>
#include <string_view>

#include "core/window_system.h"

namespace htmlppt {

// The reference monitor for window scoring and focus (config defaults.display
// .monitor). The cover itself still spans the virtual screen, so every display
// goes black; this only decides which monitor's size the show window must match.
struct ScreenSelection {
  ScreenMetrics metrics;
  // Empty on success; a message when the selector was out of range, in which
  // case metrics fall back to the primary monitor.
  std::string error;
};

// selector: "primary" (default), "secondary", or a 1-based monitor index.
ScreenSelection SelectScreenMetrics(std::string_view selector);

} // namespace htmlppt

#endif // _WIN32
