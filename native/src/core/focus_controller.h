#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "core/clock.h"
#include "core/window_system.h"

namespace htmlppt {

struct FocusTiming {
  Duration interval{100};
  Duration timeout{6000};
  // Which activation chain to use; see FocusStrategy in window_system.h.
  FocusStrategy strategy = FocusStrategy::Auto;
};

// Config strings: "auto" (default), "attach-alt", "switch". Unknown values fall
// back to Auto; config validation rejects them before a run.
FocusStrategy ParseFocusStrategy(std::string_view text);
std::string_view FocusStrategyName(FocusStrategy strategy);

class FocusController {
 public:
  FocusController(WindowSystem& windows, Clock& clock, FocusTiming timing = {});

  bool FocusEdge(std::uint32_t edge_pid);
  // Single non-blocking attempt: one window selection and one activation, no
  // retry loop and no sleep. Callers that must keep the loop's poll cadence use
  // this; FocusEdge keeps the retrying semantics for callers that want to wait.
  bool TryFocusEdge(std::uint32_t edge_pid);
  bool FocusWps(const std::vector<std::uint32_t>& wps_pids,
                ScreenMetrics screen);
  bool IsReturnSignal(std::uint32_t edge_pid);

 private:
  bool FocusWindow(WindowId id);

  WindowSystem& windows_;
  Clock& clock_;
  FocusTiming timing_;
};

} // namespace htmlppt
