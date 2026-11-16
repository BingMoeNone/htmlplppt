#include "test_main.h"

#include <string>
#include <utility>
#include <vector>

#include "core/focus_model.h"

using htmlppt::IsReturnTitle;
using htmlppt::ScreenMetrics;
using htmlppt::SelectEdgeWindow;
using htmlppt::SelectWpsShowWindow;
using htmlppt::WindowInfo;
using htmlppt::WindowRect;

namespace {

WindowInfo MakeWindow(htmlppt::WindowId id, std::uint32_t pid,
                      std::string class_name, std::string title, int w, int h,
                      bool popup, bool min_box, bool max_box) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = std::move(class_name);
  window.title = std::move(title);
  window.rect = WindowRect{0, 0, w, h};
  window.visible = true;
  window.ws_popup = popup;
  window.has_min_box = min_box;
  window.has_max_box = max_box;
  return window;
}

std::string WpsShowTitle() {
  return std::string("\xE5\xB9\xBB" "\xE7\x81\xAF" "\xE7\x89\x87"
                     "\xE6\x94\xBE" "\xE6\x98\xA0");
}

} // namespace

HTMLPPT_TEST_CASE(focus_selects_largest_edge_window_for_pid) {
  std::vector<WindowInfo> windows{
      MakeWindow(1, 100, "Chrome_WidgetWin_1", "TEF", 800, 600, true, false, false),
      MakeWindow(2, 100, "Chrome_WidgetWin_1", "TEF", 1920, 1080, true, false, false),
      MakeWindow(3, 200, "Chrome_WidgetWin_1", "Other", 1920, 1080, true, false, false),
  };
  const auto selected = SelectEdgeWindow(windows, 100);
  HTMLPPT_CHECK(selected.has_value());
  HTMLPPT_CHECK_EQ(*selected, htmlppt::WindowId{2});
}

HTMLPPT_TEST_CASE(focus_ranks_wps_show_window) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(1, 300, "Widget", "Document", 1600, 900, false, true, true),
      MakeWindow(2, 300, "screenClass", WpsShowTitle(), 1920, 1080, true, false, false),
      MakeWindow(3, 300, "Widget", "Slide Show", 1900, 1040, true, false, false),
  };
  const auto selected = SelectWpsShowWindow(windows, {300}, screen);
  HTMLPPT_CHECK(selected.has_value());
  HTMLPPT_CHECK_EQ(*selected, htmlppt::WindowId{2});
}

HTMLPPT_TEST_CASE(focus_rejects_wps_windows_without_show_traits) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(1, 300, "Widget", "Document", 1920, 1080, false, true, true),
  };
  HTMLPPT_CHECK(!SelectWpsShowWindow(windows, {300}, screen).has_value());
}

HTMLPPT_TEST_CASE(focus_recognizes_return_title) {
  HTMLPPT_CHECK(IsReturnTitle("TEF-LIVE-RETURN"));
  HTMLPPT_CHECK(IsReturnTitle("TEF-LIVE-RETURN - demo"));
  HTMLPPT_CHECK(!IsReturnTitle("TEF live"));
}

HTMLPPT_TEST_CASE(focus_ranks_wps_show_window_independent_of_order) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(3, 300, "Widget", "Slide Show", 1900, 1040, true, false, false),
      MakeWindow(2, 300, "screenClass", WpsShowTitle(), 1920, 1080, true, false, false),
      MakeWindow(1, 300, "Widget", "Document", 1600, 900, false, true, true),
  };
  const auto selected = SelectWpsShowWindow(windows, {300}, screen);
  HTMLPPT_CHECK(selected.has_value());
  HTMLPPT_CHECK_EQ(*selected, htmlppt::WindowId{2});
}

HTMLPPT_TEST_CASE(focus_accepts_exactly_four_points) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(1, 300, "Other", "Slide Show", 1920, 1080, false, true, true),
  };
  const auto selected = SelectWpsShowWindow(windows, {300}, screen);
  HTMLPPT_CHECK(selected.has_value());
  HTMLPPT_CHECK_EQ(*selected, htmlppt::WindowId{1});
}
