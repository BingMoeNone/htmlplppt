#pragma once

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <thread>

#include "core/cover_overlay.h"

namespace htmlppt {

// Layered topmost no-activate click-through cover window. The window and its
// message pump live on a dedicated thread so CoverController ticks never
// block the state machine. A foreground-event hook reasserts topmost while
// the cover is visible.
class Win32CoverOverlay final : public CoverOverlay {
 public:
  Win32CoverOverlay();
  ~Win32CoverOverlay() override;

  Win32CoverOverlay(const Win32CoverOverlay&) = delete;
  Win32CoverOverlay& operator=(const Win32CoverOverlay&) = delete;

  bool Show() override;
  void Hide() override;
  bool SetOpacity(double opacity) override;
  void ReassertTopMost() override;

  HWND hwnd() const { return hwnd_.load(); }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  static void CALLBACK ForegroundEvent(HWINEVENTHOOK hook, DWORD event,
                                       HWND hwnd, LONG object, LONG child,
                                       DWORD thread, DWORD time);
  void ThreadMain();
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void ResizeToVirtualScreen(HWND hwnd);
  void ApplyTopMost(HWND hwnd);

  std::atomic<HWND> hwnd_{nullptr};
  std::atomic<DWORD> thread_id_{0};
  bool visible_ = false;
  HANDLE ready_event_ = nullptr;
  std::thread thread_;
};

} // namespace htmlppt

#endif // _WIN32
