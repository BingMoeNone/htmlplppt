#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/window_system.h"

namespace htmlppt {

inline constexpr std::string_view kReturnTitleMarker = "TEF-LIVE-RETURN";

bool IsReturnTitle(std::string_view title);

std::optional<WindowId> SelectEdgeWindow(const std::vector<WindowInfo>& windows,
                                         std::uint32_t edge_pid);

int ScoreWpsWindow(const WindowInfo& window, ScreenMetrics screen);

std::optional<WindowId> SelectWpsShowWindow(
    const std::vector<WindowInfo>& windows,
    const std::vector<std::uint32_t>& wps_pids, ScreenMetrics screen);

} // namespace htmlppt
