#include "test_main.h"

#ifdef _WIN32
#include <string>

#include "platform/win32/screen_win32.h"

using htmlppt::SelectScreenMetrics;

HTMLPPT_TEST_CASE(win32_screen_selects_the_primary_monitor) {
  const auto selection = SelectScreenMetrics("primary");
  HTMLPPT_CHECK(selection.metrics.width > 0);
  HTMLPPT_CHECK(selection.metrics.height > 0);
  HTMLPPT_CHECK(selection.error.empty());
}

HTMLPPT_TEST_CASE(win32_screen_rejects_an_out_of_range_index) {
  const auto selection = SelectScreenMetrics("99");
  // Out of range falls back to the primary with a message, never silently.
  HTMLPPT_CHECK(selection.metrics.width > 0);
  HTMLPPT_CHECK(!selection.error.empty());
}

HTMLPPT_TEST_CASE(win32_screen_tolerates_a_missing_secondary) {
  const auto selection = SelectScreenMetrics("secondary");
  HTMLPPT_CHECK(selection.metrics.width > 0);
  // A machine with one monitor reports why it fell back.
  if (selection.error.empty()) {
    HTMLPPT_CHECK(selection.metrics.width > 0);
  } else {
    HTMLPPT_CHECK(selection.error.find("secondary") != std::string::npos);
  }
}
#endif