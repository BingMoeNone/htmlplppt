#include "test_main.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/clock.h"
#include "core/focus_controller.h"
#include "core/window_system.h"

using htmlppt::ClockPoint;
using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::FocusController;
using htmlppt::FocusTiming;
using htmlppt::ScreenMetrics;
using htmlppt::WindowId;
using htmlppt::WindowInfo;
using htmlppt::WindowRect;
using htmlppt::WindowSystem;

namespace {

class FakeWindows final : public WindowSystem {
 public:
  std::vector<WindowInfo> Enumerate() override { return windows; }
  WindowId Foreground() override { return foreground; }
  bool IsForeground(WindowId id) override { return id == foreground; }
  bool Activate(WindowId id) override {
    ++activation_count;
    if (!allow_activate) return false;
    if (activation_index < activation_results.size()) {
      const bool allowed = activation_results[activation_index];
      ++activation_index;
      if (!allowed) return false;
    }
    foreground = id;
    return true;
  }
  bool ActivateWith(WindowId id, htmlppt::FocusStrategy strategy) override {
    strategies.push_back(strategy);
    return Activate(id);
  }
  bool IsWindow(WindowId id) override {
    for (const auto& window : windows) {
      if (window.id == id) return true;
    }
    return false;
  }

  std::vector<WindowInfo> windows;
  WindowId foreground = 0;
  bool allow_activate = true;
  std::vector<bool> activation_results;
  std::vector<htmlppt::FocusStrategy> strategies;
  std::size_t activation_index = 0;
  int activation_count = 0;
};

WindowInfo MakeEdge(WindowId id, std::uint32_t pid, int width, int height) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = "Chrome_WidgetWin_1";
  window.title = "TEF";
  window.rect = WindowRect{0, 0, width, height};
  window.visible = true;
  return window;
}

WindowInfo MakeWps(WindowId id, std::uint32_t pid, bool return_title = false) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = "screenClass";
  window.title = return_title ? "TEF-LIVE-RETURN" : "Slide Show";
  window.rect = WindowRect{0, 0, 1920, 1080};
  window.visible = true;
  window.ws_popup = true;
  window.has_min_box = false;
  window.has_max_box = false;
  return window;
}

} // namespace

HTMLPPT_TEST_CASE(focus_controller_activates_edge_window) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600), MakeEdge(2, 100, 1920, 1080)};
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(controller.FocusEdge(100));
  HTMLPPT_CHECK_EQ(windows.foreground, WindowId{2});
}

HTMLPPT_TEST_CASE(focus_controller_times_out_when_activation_is_denied) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600)};
  windows.allow_activate = false;
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{50}});
  HTMLPPT_CHECK(!controller.FocusEdge(100));
}

HTMLPPT_TEST_CASE(focus_controller_activates_wps_show_window) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeWps(1, 300)};
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(controller.FocusWps({300}, ScreenMetrics{1920, 1080}));
  HTMLPPT_CHECK_EQ(windows.foreground, WindowId{1});
}

HTMLPPT_TEST_CASE(focus_controller_detects_return_title) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeWps(1, 100, true)};
  FocusController controller(windows, clock);
  HTMLPPT_CHECK(controller.IsReturnSignal(100));
  HTMLPPT_CHECK(!controller.IsReturnSignal(200));
}

HTMLPPT_TEST_CASE(focus_controller_retries_until_success) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600)};
  windows.activation_results = {false, false, true};
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(controller.FocusEdge(100));
  HTMLPPT_CHECK_EQ(windows.activation_count, 3);
  HTMLPPT_CHECK_EQ(windows.foreground, WindowId{1});
}

HTMLPPT_TEST_CASE(focus_controller_return_signal_is_pid_scoped) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeWps(1, 100), MakeWps(2, 200, true)};
  FocusController controller(windows, clock);
  HTMLPPT_CHECK(!controller.IsReturnSignal(100));
  HTMLPPT_CHECK(controller.IsReturnSignal(200));
}

HTMLPPT_TEST_CASE(focus_controller_zero_interval_still_times_out) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600)};
  windows.allow_activate = false;
  FocusController controller(windows, clock, FocusTiming{Duration{0}, Duration{50}});
  HTMLPPT_CHECK(!controller.FocusEdge(100));
  HTMLPPT_CHECK(clock.now() >= ClockPoint(Duration{50}));
}

// TryFocusEdge is the single non-blocking attempt the worker loop uses: one
// selection pass, one activation, no retry loop and no sleep.
HTMLPPT_TEST_CASE(focus_controller_try_focus_edge_activates_once) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600), MakeEdge(2, 100, 1920, 1080)};
  FocusController controller(windows, clock,
                             FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(controller.TryFocusEdge(100));
  HTMLPPT_CHECK_EQ(windows.foreground, WindowId{2});
  HTMLPPT_CHECK_EQ(windows.activation_count, 1);
  HTMLPPT_CHECK(clock.sleeps().empty());
}

HTMLPPT_TEST_CASE(focus_controller_try_focus_edge_does_not_wait_for_a_window) {
  FakeClock clock;
  FakeWindows windows;
  FocusController controller(windows, clock,
                             FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(!controller.TryFocusEdge(100));
  HTMLPPT_CHECK(clock.sleeps().empty());
  HTMLPPT_CHECK_EQ(clock.now(), ClockPoint(Duration{0}));
}

HTMLPPT_TEST_CASE(focus_controller_uses_the_configured_strategy) {
  FakeWindows windows;
  FakeClock clock;
  WindowInfo window;
  window.id = 11;
  window.pid = 42;
  window.visible = true;
  window.class_name = "screenClass";
  window.ws_popup = true;
  window.has_min_box = false;
  window.has_max_box = false;
  window.rect = WindowRect{0, 0, 1920, 1080};
  windows.windows = {window};

  FocusTiming timing;
  timing.strategy = htmlppt::FocusStrategy::AttachAlt;
  FocusController controller(windows, clock, timing);
  HTMLPPT_CHECK(controller.FocusWps({42}, ScreenMetrics{1920, 1080}));
  HTMLPPT_CHECK_EQ(windows.strategies.size(), std::size_t{1});
  HTMLPPT_CHECK(windows.strategies[0] == htmlppt::FocusStrategy::AttachAlt);

  windows.strategies.clear();
  timing.strategy = htmlppt::FocusStrategy::Switch;
  FocusController second(windows, clock, timing);
  HTMLPPT_CHECK(second.FocusWps({42}, ScreenMetrics{1920, 1080}));
  HTMLPPT_CHECK(windows.strategies[0] == htmlppt::FocusStrategy::Switch);
}

// The worker re-applies a target's focus settings at every trigger instead of
// rebuilding the controller, so SetTiming must take effect for the next attempt.
HTMLPPT_TEST_CASE(focus_controller_set_timing_updates_strategy_and_timeout) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600)};
  windows.allow_activate = false;
  FocusController controller(
      windows, clock, FocusTiming{Duration{10}, Duration{100}});

  FocusTiming timing{Duration{10}, Duration{30}, htmlppt::FocusStrategy::Switch};
  controller.SetTiming(timing);
  HTMLPPT_CHECK(!controller.FocusEdge(100));
  // The 30 ms override bounds the attempt, not the constructor's 100 ms.
  HTMLPPT_CHECK(clock.now() >= ClockPoint(Duration{30}));
  HTMLPPT_CHECK(clock.now() < ClockPoint(Duration{100}));
  HTMLPPT_CHECK(!windows.strategies.empty());
  HTMLPPT_CHECK(windows.strategies.back() == htmlppt::FocusStrategy::Switch);
}

HTMLPPT_TEST_CASE(focus_strategy_parses_config_strings) {
  HTMLPPT_CHECK(htmlppt::ParseFocusStrategy("auto") == htmlppt::FocusStrategy::Auto);
  HTMLPPT_CHECK(htmlppt::ParseFocusStrategy("attach-alt") == htmlppt::FocusStrategy::AttachAlt);
  HTMLPPT_CHECK(htmlppt::ParseFocusStrategy("switch") == htmlppt::FocusStrategy::Switch);
  HTMLPPT_CHECK(htmlppt::ParseFocusStrategy("bogus") == htmlppt::FocusStrategy::Auto);
  HTMLPPT_CHECK_EQ(htmlppt::FocusStrategyName(htmlppt::FocusStrategy::AttachAlt), std::string_view{"attach-alt"});
}