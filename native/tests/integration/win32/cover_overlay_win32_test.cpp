#include "test_main.h"

#ifdef _WIN32

#include <windows.h>

#include "platform/win32/cover_overlay_win32.h"

using htmlppt::Win32CoverOverlay;

namespace {

int LayeredAlpha(HWND hwnd) {
  COLORREF color = 0;
  BYTE alpha = 0;
  DWORD flags = 0;
  if (GetLayeredWindowAttributes(hwnd, &color, &alpha, &flags) == 0) {
    return -1;
  }
  return (flags & LWA_ALPHA) != 0 ? static_cast<int>(alpha) : -1;
}

} // namespace

HTMLPPT_TEST_CASE(win32_cover_overlay_shows_layered_topmost_window) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.hwnd() != nullptr);
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  HTMLPPT_CHECK(IsWindowVisible(hwnd) != 0);

  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  HTMLPPT_CHECK((ex_style & WS_EX_LAYERED) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_TOOLWINDOW) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_NOACTIVATE) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_TRANSPARENT) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_TOPMOST) != 0);
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  HTMLPPT_CHECK((style & WS_POPUP) != 0);

  RECT rect{};
  HTMLPPT_CHECK(GetWindowRect(hwnd, &rect) != 0);
  HTMLPPT_CHECK(rect.right - rect.left > 0);
  HTMLPPT_CHECK(rect.bottom - rect.top > 0);
  overlay.Hide();
}

HTMLPPT_TEST_CASE(win32_cover_overlay_updates_and_clamps_opacity) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  HTMLPPT_CHECK(overlay.SetOpacity(0.5));
  HTMLPPT_CHECK_EQ(LayeredAlpha(hwnd), 128);
  HTMLPPT_CHECK(overlay.SetOpacity(1.5));
  HTMLPPT_CHECK_EQ(LayeredAlpha(hwnd), 255);
  HTMLPPT_CHECK(overlay.SetOpacity(-1.0));
  HTMLPPT_CHECK_EQ(LayeredAlpha(hwnd), 0);
  overlay.Hide();
}

HTMLPPT_TEST_CASE(win32_cover_overlay_hides_and_reshows) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  overlay.Hide();
  HTMLPPT_CHECK(IsWindowVisible(hwnd) == 0);
  HTMLPPT_CHECK(overlay.Show());
  HTMLPPT_CHECK(IsWindowVisible(hwnd) != 0);
  overlay.Hide();
}

HTMLPPT_TEST_CASE(win32_cover_overlay_reasserts_topmost) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  HTMLPPT_CHECK((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);
  overlay.ReassertTopMost();
  HTMLPPT_CHECK((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
  overlay.Hide();
}

#endif // _WIN32
