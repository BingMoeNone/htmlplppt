#include "platform/win32/cover_overlay_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <memory>
#include <utility>

namespace htmlppt {
namespace {

constexpr UINT kShowMessage = WM_APP + 1;
constexpr UINT kHideMessage = WM_APP + 2;
constexpr UINT kOpacityMessage = WM_APP + 3;
constexpr UINT kTopMostMessage = WM_APP + 4;
constexpr DWORD kStartupWaitMilliseconds = 5000;
constexpr DWORD kShutdownWaitMilliseconds = 2000;
constexpr UINT kSendTimeoutMilliseconds = 100;
constexpr wchar_t kOverlayClass[] = L"HtmlpptCoverOverlay";
std::atomic<HWND> g_overlay_hwnd{nullptr};

} // namespace

struct Win32CoverOverlay::State {
  std::atomic<HWND> hwnd{nullptr};
  std::atomic<DWORD> thread_id{0};
  std::atomic<bool> stop{false};
  // OS signals for the runtime: a resume and a display-topology change. The
  // window thread sets them; the worker thread consumes them once.
  std::atomic<bool> resume_pending{false};
  std::atomic<bool> display_pending{false};
  std::atomic<int> display_width{0};
  std::atomic<int> display_height{0};
  bool visible = false;
  HANDLE ready_event = nullptr;
  HANDLE exit_event = nullptr;

  ~State() {
    if (ready_event != nullptr) CloseHandle(ready_event);
    if (exit_event != nullptr) CloseHandle(exit_event);
  }
};

Win32CoverOverlay::Win32CoverOverlay() {
  auto state = std::make_shared<State>();
  state->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  state->exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (state->ready_event == nullptr || state->exit_event == nullptr) {
    state.reset();
    return;
  }
  state_ = std::move(state);
  thread_ = std::thread(&Win32CoverOverlay::ThreadMain, state_);
  if (WaitForSingleObject(state_->ready_event, kStartupWaitMilliseconds) !=
      WAIT_OBJECT_0) {
    state_->stop.store(true);
    const DWORD id = state_->thread_id.load();
    if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
  }
}

Win32CoverOverlay::~Win32CoverOverlay() {
  if (state_ == nullptr) {
    if (thread_.joinable()) thread_.join();
    return;
  }
  state_->stop.store(true);
  const DWORD id = state_->thread_id.load();
  if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
  if (WaitForSingleObject(state_->exit_event, kShutdownWaitMilliseconds) ==
      WAIT_OBJECT_0) {
    if (thread_.joinable()) thread_.join();
  } else if (thread_.joinable()) {
    // Never TerminateThread: the worker keeps its own shared state alive and
    // never touches the overlay object after detaching.
    thread_.detach();
  }
  state_.reset();
}

HWND Win32CoverOverlay::hwnd() const {
  return state_ != nullptr ? state_->hwnd.load() : nullptr;
}

bool Win32CoverOverlay::Show() {
  const HWND window = hwnd();
  if (window == nullptr) return false;
  DWORD_PTR result = 0;
  return SendMessageTimeoutW(window, kShowMessage, 0, 0, SMTO_ABORTIFHUNG,
                             kSendTimeoutMilliseconds, &result) != 0 &&
         result != 0;
}

void Win32CoverOverlay::Hide() {
  const HWND window = hwnd();
  if (window == nullptr) return;
  DWORD_PTR result = 0;
  SendMessageTimeoutW(window, kHideMessage, 0, 0, SMTO_ABORTIFHUNG,
                      kSendTimeoutMilliseconds, &result);
}

bool Win32CoverOverlay::SetOpacity(double opacity) {
  const HWND window = hwnd();
  if (window == nullptr) return false;
  if (!(opacity >= 0.0)) opacity = 0.0;
  if (opacity > 1.0) opacity = 1.0;
  const BYTE alpha = static_cast<BYTE>(opacity * 255.0 + 0.5);
  DWORD_PTR result = 0;
  return SendMessageTimeoutW(window, kOpacityMessage, alpha, 0,
                             SMTO_ABORTIFHUNG, kSendTimeoutMilliseconds,
                             &result) != 0 &&
         result != 0;
}

void Win32CoverOverlay::ReassertTopMost() {
  const HWND window = hwnd();
  if (window == nullptr) return;
  DWORD_PTR result = 0;
  SendMessageTimeoutW(window, kTopMostMessage, 0, 0, SMTO_ABORTIFHUNG,
                      kSendTimeoutMilliseconds, &result);
}

void Win32CoverOverlay::ThreadMain(std::shared_ptr<State> state) {
  // Force creation of this thread's message queue before publishing the
  // thread id, so a shutdown post can never be lost.
  MSG probe{};
  PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  state->thread_id.store(GetCurrentThreadId());

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = &Win32CoverOverlay::WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  // IDC_ARROW; the SDK macro is ANSI in non-UNICODE builds.
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = kOverlayClass;
  if (state->stop.load()) {
    SetEvent(state->ready_event);
    SetEvent(state->exit_event);
    return;
  }
  window_class.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
  if (RegisterClassW(&window_class) == 0) {
    DeleteObject(window_class.hbrBackground);
    SetEvent(state->ready_event);
    SetEvent(state->exit_event);
    return;
  }

  HWND hwnd = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
          WS_EX_TRANSPARENT | WS_EX_TOPMOST,
      kOverlayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
      window_class.hInstance, state.get());
  if (hwnd == nullptr) {
    UnregisterClassW(kOverlayClass, window_class.hInstance);
    DeleteObject(window_class.hbrBackground);
    SetEvent(state->ready_event);
    SetEvent(state->exit_event);
    return;
  }

  SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
  state->hwnd.store(hwnd);
  g_overlay_hwnd.store(hwnd);
  const HWINEVENTHOOK hook = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      &Win32CoverOverlay::ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
  SetEvent(state->ready_event);

  MSG message{};
  if (!state->stop.load()) {
    while (!state->stop.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  if (hook != nullptr) UnhookWinEvent(hook);
  g_overlay_hwnd.store(nullptr);
  state->hwnd.store(nullptr);
  DestroyWindow(hwnd);
  UnregisterClassW(kOverlayClass, window_class.hInstance);
  DeleteObject(window_class.hbrBackground);
  SetEvent(state->exit_event);
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
    auto* state = static_cast<State*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    return TRUE;
  }
  auto* state = reinterpret_cast<State*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (state != nullptr) {
    return HandleMessage(*state, hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Win32CoverOverlay::HandleMessage(State& state, HWND hwnd, UINT message,
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
      state.visible = true;
      return TRUE;
    case kHideMessage:
      ShowWindow(hwnd, SW_HIDE);
      state.visible = false;
      return TRUE;
    case kOpacityMessage:
      if (!state.visible) return FALSE;
      return SetLayeredWindowAttributes(hwnd, 0,
                                        static_cast<BYTE>(wparam),
                                        LWA_ALPHA) != 0
                 ? TRUE
                 : FALSE;
    case kTopMostMessage:
      if (state.visible) ApplyTopMost(hwnd);
      return TRUE;
    case WM_DISPLAYCHANGE:
      ResizeToVirtualScreen(hwnd);
      if (state.visible) ApplyTopMost(hwnd);
      state.display_width.store(GetSystemMetrics(SM_CXSCREEN),
                                std::memory_order_relaxed);
      state.display_height.store(GetSystemMetrics(SM_CYSCREEN),
                                 std::memory_order_relaxed);
      state.display_pending.store(true, std::memory_order_relaxed);
      return 0;
    case WM_DPICHANGED:
      // A DPI change is a display-topology change for the cover: re-cover the
      // new virtual screen instead of leaving a black band or a stale size.
      ResizeToVirtualScreen(hwnd);
      if (state.visible) ApplyTopMost(hwnd);
      state.display_width.store(GetSystemMetrics(SM_CXSCREEN),
                                std::memory_order_relaxed);
      state.display_height.store(GetSystemMetrics(SM_CYSCREEN),
                                 std::memory_order_relaxed);
      state.display_pending.store(true, std::memory_order_relaxed);
      return 0;
    case WM_POWERBROADCAST:
      if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
        state.resume_pending.store(true, std::memory_order_relaxed);
      }
      // The loop's wait now pumps messages, so a resume reaches the overlay
      // directly. QPC jumps across sleep, so re-assert the cover geometry and
      // topmost instead of trusting the pre-sleep placement.
      if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
        ResizeToVirtualScreen(hwnd);
        if (state.visible) ApplyTopMost(hwnd);
      }
      return TRUE;
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

bool Win32CoverOverlay::ConsumeResumeEvent() {
  return state_->resume_pending.exchange(false, std::memory_order_relaxed);
}

bool Win32CoverOverlay::ConsumeDisplayChange(ScreenMetrics* metrics) {
  if (!state_->display_pending.exchange(false, std::memory_order_relaxed)) {
    return false;
  }
  if (metrics != nullptr) {
    metrics->width = state_->display_width.load(std::memory_order_relaxed);
    metrics->height = state_->display_height.load(std::memory_order_relaxed);
  }
  return true;
}

} // namespace htmlppt

#endif // _WIN32
