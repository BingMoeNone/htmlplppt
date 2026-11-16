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
