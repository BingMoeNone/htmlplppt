#include "platform/win32/window_system_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/win32/utf8_win32.h"

namespace htmlppt {
namespace {

HWND ToWindow(WindowId id) { return reinterpret_cast<HWND>(id); }
WindowId ToId(HWND hwnd) { return reinterpret_cast<WindowId>(hwnd); }

std::string WindowText(HWND hwnd) {
  const int length = GetWindowTextLengthW(hwnd);
  if (length <= 0) return {};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied =
      GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
  if (copied <= 0) return {};
  text.resize(static_cast<std::size_t>(copied));
  return WideToUtf8(text).value_or(std::string());
}

std::string ClassName(HWND hwnd) {
  wchar_t buffer[256] = {};
  const int copied = GetClassNameW(hwnd, buffer, 256);
  if (copied <= 0) return {};
  const std::wstring_view name(buffer, static_cast<std::size_t>(copied));
  return WideToUtf8(name).value_or(std::string());
}

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM parameter) {
  auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(parameter);
  WindowInfo info;
  info.id = ToId(hwnd);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  info.pid = static_cast<std::uint32_t>(pid);
  info.class_name = ClassName(hwnd);
  info.title = WindowText(hwnd);
  RECT rect{};
  if (GetWindowRect(hwnd, &rect) != 0) {
    info.rect = WindowRect{rect.left, rect.top, rect.right, rect.bottom};
  }
  info.visible = IsWindowVisible(hwnd) != 0;
  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  info.topmost = (ex_style & WS_EX_TOPMOST) != 0;
  info.ws_popup = (style & WS_POPUP) != 0;
  info.has_min_box = (style & WS_MINIMIZEBOX) != 0;
  info.has_max_box = (style & WS_MAXIMIZEBOX) != 0;
  windows->push_back(std::move(info));
  return TRUE;
}

bool IsForegroundWindow(HWND hwnd) {
  return hwnd != nullptr && IsWindowVisible(hwnd) != 0 &&
         GetForegroundWindow() == hwnd;
}

} // namespace

ScreenMetrics PrimaryScreenMetrics() {
  ScreenMetrics metrics;
  metrics.width = GetSystemMetrics(SM_CXSCREEN);
  metrics.height = GetSystemMetrics(SM_CYSCREEN);
  return metrics;
}

std::vector<WindowInfo> Win32WindowSystem::Enumerate() {
  std::vector<WindowInfo> windows;
  EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&windows));
  return windows;
}

WindowId Win32WindowSystem::Foreground() {
  return ToId(GetForegroundWindow());
}

bool Win32WindowSystem::IsForeground(WindowId id) {
  return IsForegroundWindow(ToWindow(id));
}

bool Win32WindowSystem::IsWindow(WindowId id) {
  return id != 0 && ::IsWindow(ToWindow(id)) != 0;
}

bool Win32WindowSystem::Activate(WindowId id) {
  const HWND hwnd = ToWindow(id);
  if (hwnd == nullptr || ::IsWindow(hwnd) == 0) return false;

  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
  if (IsIconic(hwnd) != 0) ShowWindow(hwnd, SW_RESTORE);
  if (IsForegroundWindow(hwnd)) return true;

  SetForegroundWindow(hwnd);
  if (IsForegroundWindow(hwnd)) return true;

  SwitchToThisWindow(hwnd, TRUE);
  if (IsForegroundWindow(hwnd)) return true;

  const HWND foreground = GetForegroundWindow();
  const DWORD foreground_thread =
      foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
  const DWORD current_thread = GetCurrentThreadId();
  const bool attached_foreground =
      foreground_thread != 0 && foreground_thread != current_thread &&
      AttachThreadInput(current_thread, foreground_thread, TRUE) != 0;
  const bool attached_target =
      target_thread != 0 && target_thread != current_thread &&
      target_thread != foreground_thread &&
      AttachThreadInput(current_thread, target_thread, TRUE) != 0;
  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  if (attached_target) AttachThreadInput(current_thread, target_thread, FALSE);
  if (attached_foreground) {
    AttachThreadInput(current_thread, foreground_thread, FALSE);
  }
  if (IsForegroundWindow(hwnd)) return true;

  keybd_event(VK_MENU, 0, 0, 0);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  const bool alt_unlocked = IsForegroundWindow(hwnd);
  keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
  if (alt_unlocked) return true;

  SetForegroundWindow(hwnd);
  return IsForegroundWindow(hwnd);
}

} // namespace htmlppt

#endif // _WIN32
