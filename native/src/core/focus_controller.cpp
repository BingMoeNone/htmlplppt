#include "core/focus_controller.h"

#include <algorithm>

#include "core/focus_model.h"

namespace htmlppt {

FocusController::FocusController(WindowSystem& windows, Clock& clock,
                                 FocusTiming timing)
    : windows_(windows), clock_(clock) {
  SetTiming(timing);
}

void FocusController::SetTiming(FocusTiming timing) {
  timing_ = timing;
  if (timing_.interval < Duration{1}) {
    timing_.interval = Duration{1};
  }
}

FocusStrategy ParseFocusStrategy(std::string_view text) {
  if (text == "attach-alt") return FocusStrategy::AttachAlt;
  if (text == "switch") return FocusStrategy::Switch;
  return FocusStrategy::Auto;
}

std::string_view FocusStrategyName(FocusStrategy strategy) {
  switch (strategy) {
    case FocusStrategy::AttachAlt: return "attach-alt";
    case FocusStrategy::Switch: return "switch";
    case FocusStrategy::Auto: break;
  }
  return "auto";
}

bool FocusController::FocusWindow(WindowId id) {
  if (!windows_.ActivateWith(id, timing_.strategy)) return false;
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
