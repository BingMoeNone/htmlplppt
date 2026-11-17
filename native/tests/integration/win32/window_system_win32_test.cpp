#include "test_main.h"

#ifdef _WIN32

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "platform/win32/window_system_win32.h"

using htmlppt::PrimaryScreenMetrics;
using htmlppt::WindowId;
using htmlppt::WindowInfo;
using htmlppt::Win32WindowSystem;

namespace {

constexpr wchar_t kTestClassName[] = L"HtmlpptWindowSystemTest";

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                LPARAM lparam) {
  if (message == WM_DESTROY) return 0;
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void RegisterTestClass() {
  static std::once_flag once;
  std::call_once(once, [] {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = TestWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    // IDC_ARROW; the SDK macro is ANSI in non-UNICODE builds.
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kTestClassName;
    RegisterClassW(&window_class);
  });
}

HWND MakeTestWindow(const wchar_t* title, DWORD style, DWORD ex_style,
                    bool show) {
  RegisterTestClass();
  HWND hwnd = CreateWindowExW(ex_style, kTestClassName, title, style, 100, 100,
                              640, 480, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (show) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
  }
  return hwnd;
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

const WindowInfo* FindWindow(const std::vector<WindowInfo>& windows,
                             WindowId id) {
  for (const auto& window : windows) {
    if (window.id == id) return &window;
  }
  return nullptr;
}

bool InteractiveWindowStation() {
  USEROBJECTFLAGS flags{};
  DWORD length = 0;
  return GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
                                   &flags, sizeof(flags),
                                   &length) != 0 &&
         (flags.dwFlags & WSF_VISIBLE) != 0;
}

} // namespace

HTMLPPT_TEST_CASE(win32_window_system_enumerates_visible_window) {
  const HWND hwnd = MakeTestWindow(L"htmlppt window title",
                                   WS_OVERLAPPEDWINDOW, 0, true);
  HTMLPPT_CHECK(hwnd != nullptr);
  PumpMessages();

  Win32WindowSystem windows;
  const auto listed = windows.Enumerate();
  const auto* found = FindWindow(listed, reinterpret_cast<WindowId>(hwnd));
  HTMLPPT_CHECK(found != nullptr);
  HTMLPPT_CHECK_EQ(found->pid,
                   static_cast<std::uint32_t>(GetCurrentProcessId()));
  HTMLPPT_CHECK_EQ(found->title, std::string("htmlppt window title"));
  HTMLPPT_CHECK_EQ(found->class_name, std::string("HtmlpptWindowSystemTest"));
  HTMLPPT_CHECK(found->visible);
  HTMLPPT_CHECK(found->rect.width() > 0);
  HTMLPPT_CHECK(found->rect.height() > 0);
  HTMLPPT_CHECK(!found->topmost);
  HTMLPPT_CHECK(!found->ws_popup);
  HTMLPPT_CHECK(found->has_min_box);
  HTMLPPT_CHECK(found->has_max_box);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_reports_popup_and_topmost_styles) {
  const HWND hwnd =
      MakeTestWindow(L"popup", WS_POPUP, WS_EX_TOPMOST, true);
  HTMLPPT_CHECK(hwnd != nullptr);
  PumpMessages();

  Win32WindowSystem windows;
  const auto listed = windows.Enumerate();
  const auto* found = FindWindow(listed, reinterpret_cast<WindowId>(hwnd));
  HTMLPPT_CHECK(found != nullptr);
  HTMLPPT_CHECK(found->topmost);
  HTMLPPT_CHECK(found->ws_popup);
  HTMLPPT_CHECK(!found->has_min_box);
  HTMLPPT_CHECK(!found->has_max_box);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_marks_hidden_window_invisible) {
  const HWND hwnd =
      MakeTestWindow(L"hidden", WS_OVERLAPPEDWINDOW, 0, false);
  HTMLPPT_CHECK(hwnd != nullptr);

  Win32WindowSystem windows;
  const auto listed = windows.Enumerate();
  const auto* found = FindWindow(listed, reinterpret_cast<WindowId>(hwnd));
  HTMLPPT_CHECK(found != nullptr);
  HTMLPPT_CHECK(!found->visible);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_activates_own_window) {
  if (!InteractiveWindowStation()) return;
  const HWND original = GetForegroundWindow();
  const HWND hwnd =
      MakeTestWindow(L"activate", WS_OVERLAPPEDWINDOW, 0, true);
  HTMLPPT_CHECK(hwnd != nullptr);
  PumpMessages();

  Win32WindowSystem windows;
  const WindowId id = reinterpret_cast<WindowId>(hwnd);
  HTMLPPT_CHECK(windows.IsWindow(id));
  HTMLPPT_CHECK(windows.Activate(id));
  HTMLPPT_CHECK(windows.IsForeground(id));
  HTMLPPT_CHECK_EQ(windows.Foreground(), id);

  if (original != nullptr) SetForegroundWindow(original);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_rejects_invalid_window) {
  Win32WindowSystem windows;
  HTMLPPT_CHECK(!windows.IsWindow(0));
  HTMLPPT_CHECK(!windows.IsForeground(0));
  HTMLPPT_CHECK(!windows.Activate(0));
}

HTMLPPT_TEST_CASE(win32_window_system_reports_primary_screen) {
  const auto screen = PrimaryScreenMetrics();
  HTMLPPT_CHECK(screen.width > 0);
  HTMLPPT_CHECK(screen.height > 0);
}

#endif // _WIN32
