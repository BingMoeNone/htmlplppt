#include "platform/win32/cover_overlay_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <utility>

namespace htmlppt {
namespace {

constexpr UINT kShowMessage = WM_APP + 1;
constexpr UINT kHideMessage = WM_APP + 2;
constexpr UINT kOpacityMessage = WM_APP + 3;
constexpr UINT kTopMostMessage = WM_APP + 4;
constexpr wchar_t kOverlayClass[] = L"HtmlpptCoverOverlay";
std::atomic<HWND> g_overlay_hwnd{nullptr};

} // namespace

Win32CoverOverlay::Win32CoverOverlay() {
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ready_event_ == nullptr) return;
  thread_ = std::thread(&Win32CoverOverlay::ThreadMain, this);
  WaitForSingleObject(ready_event_, 5000);
}

Win32CoverOverlay::~Win32CoverOverlay() {
  if (thread_.joinable()) {
    const DWORD id = thread_id_.load();
    if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
    thread_.join();
  }
  if (ready_event_ != nullptr) CloseHandle(ready_event_);
}

bool Win32CoverOverlay::Show() {
  const HWND hwnd = hwnd_.load();
  if (hwnd == nullptr) return false;
  return SendMessageW(hwnd, kShowMessage, 0, 0) != 0;
}

void Win32CoverOverlay::Hide() {
  const HWND hwnd = hwnd_.load();
  if (hwnd != nullptr) SendMessageW(hwnd, kHideMessage, 0, 0);
}

bool Win32CoverOverlay::SetOpacity(double opacity) {
  const HWND hwnd = hwnd_.load();
  if (hwnd == nullptr) return false;
  if (opacity < 0.0) opacity = 0.0;
  if (opacity > 1.0) opacity = 1.0;
  const BYTE alpha = static_cast<BYTE>(opacity * 255.0 + 0.5);
  return SendMessageW(hwnd, kOpacityMessage, alpha, 0) != 0;
}

void Win32CoverOverlay::ReassertTopMost() {
  const HWND hwnd = hwnd_.load();
  if (hwnd != nullptr) SendMessageW(hwnd, kTopMostMessage, 0, 0);
}

void Win32CoverOverlay::ThreadMain() {
  thread_id_.store(GetCurrentThreadId());
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = &Win32CoverOverlay::WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  // IDC_ARROW; the SDK macro is ANSI in non-UNICODE builds.
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
  window_class.lpszClassName = kOverlayClass;
  if (RegisterClassW(&window_class) == 0) {
    SetEvent(ready_event_);
    return;
  }

  HWND hwnd = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
          WS_EX_TRANSPARENT | WS_EX_TOPMOST,
      kOverlayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
      window_class.hInstance, this);
  if (hwnd == nullptr) {
    UnregisterClassW(kOverlayClass, window_class.hInstance);
    DeleteObject(window_class.hbrBackground);
    SetEvent(ready_event_);
    return;
  }

  SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
  hwnd_.store(hwnd);
  g_overlay_hwnd.store(hwnd);
  const HWINEVENTHOOK hook = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      &Win32CoverOverlay::ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
  SetEvent(ready_event_);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (hook != nullptr) UnhookWinEvent(hook);
  g_overlay_hwnd.store(nullptr);
  hwnd_.store(nullptr);
  DestroyWindow(hwnd);
  UnregisterClassW(kOverlayClass, window_class.hInstance);
  DeleteObject(window_class.hbrBackground);
}

void Win32CoverOverlay::ResizeToVirtualScreen(HWND hwnd) {
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  SetWindowPos(hwnd, nullptr, left, top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32CoverOverlay::ApplyTopMost(HWND hwnd) {
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK Win32CoverOverlay::WindowProc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    auto* self = reinterpret_cast<Win32CoverOverlay*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return TRUE;
  }
  auto* self = reinterpret_cast<Win32CoverOverlay*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Win32CoverOverlay::HandleMessage(HWND hwnd, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(hwnd, &paint);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(hwnd, &paint);
      return 0;
    }
    case kShowMessage:
      ResizeToVirtualScreen(hwnd);
      SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      ApplyTopMost(hwnd);
      visible_ = true;
      return TRUE;
    case kHideMessage:
      ShowWindow(hwnd, SW_HIDE);
      visible_ = false;
      return TRUE;
    case kOpacityMessage:
      if (!visible_) return FALSE;
      return SetLayeredWindowAttributes(hwnd, 0,
                                        static_cast<BYTE>(wparam),
                                        LWA_ALPHA) != 0
                 ? TRUE
                 : FALSE;
    case kTopMostMessage:
      if (visible_) ApplyTopMost(hwnd);
      return TRUE;
    case WM_DISPLAYCHANGE:
      ResizeToVirtualScreen(hwnd);
      if (visible_) ApplyTopMost(hwnd);
      return 0;
    case WM_CLOSE:
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

void CALLBACK Win32CoverOverlay::ForegroundEvent(HWINEVENTHOOK, DWORD, HWND,
                                                 LONG, LONG, DWORD, DWORD) {
  const HWND hwnd = g_overlay_hwnd.load();
  if (hwnd == nullptr || IsWindowVisible(hwnd) == 0) return;
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace htmlppt

#endif // _WIN32
