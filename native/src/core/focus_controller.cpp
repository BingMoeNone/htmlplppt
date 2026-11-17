#include "core/focus_controller.h"

#include <algorithm>

#include "core/focus_model.h"

namespace htmlppt {

FocusController::FocusController(WindowSystem& windows, Clock& clock,
                                 FocusTiming timing)
    : windows_(windows), clock_(clock), timing_(timing) {
  if (timing_.interval < Duration{1}) {
    timing_.interval = Duration{1};
  }
}

bool FocusController::FocusWindow(WindowId id) {
  if (!windows_.Activate(id)) return false;
  return windows_.IsForeground(id);
}

bool FocusController::FocusEdge(std::uint32_t edge_pid) {
  const auto deadline = clock_.now() + timing_.timeout;
  while (clock_.now() < deadline) {
    const auto selected = SelectEdgeWindow(windows_.Enumerate(), edge_pid);
    if (selected.has_value() && FocusWindow(*selected)) return true;
    clock_.sleep_until(std::min(clock_.now() + timing_.interval, deadline));
  }
  return false;
}

bool FocusController::TryFocusEdge(std::uint32_t edge_pid) {
  const auto selected = SelectEdgeWindow(windows_.Enumerate(), edge_pid);
  if (!selected.has_value()) return false;
  return FocusWindow(*selected);
}

bool FocusController::FocusWps(const std::vector<std::uint32_t>& wps_pids,
                               ScreenMetrics screen) {
  const auto deadline = clock_.now() + timing_.timeout;
  while (clock_.now() < deadline) {
    const auto selected =
        SelectWpsShowWindow(windows_.Enumerate(), wps_pids, screen);
    if (selected.has_value() && FocusWindow(*selected)) return true;
    clock_.sleep_until(std::min(clock_.now() + timing_.interval, deadline));
  }
  return false;
}

bool FocusController::IsReturnSignal(std::uint32_t edge_pid) {
  for (const auto& window : windows_.Enumerate()) {
    if (window.pid == edge_pid && IsReturnTitle(window.title)) return true;
  }
  return false;
}

} // namespace htmlppt
