#pragma once

#ifdef _WIN32

#include <windows.h>

#include <memory>
#include <thread>

#include "core/cover_overlay.h"

namespace htmlppt {

// Layered topmost no-activate click-through cover window. The window and its
// message pump live on a dedicated thread so CoverController ticks never
// block the state machine. A foreground-event hook reasserts topmost while
// the cover is visible. Thread-owned state lives in a heap object shared with
// the worker, so a detached worker never touches this overlay after
// destruction.
class Win32CoverOverlay final : public CoverOverlay {
 public:
  Win32CoverOverlay();
  ~Win32CoverOverlay() override;

  Win32CoverOverlay(const Win32CoverOverlay&) = delete;
  Win32CoverOverlay& operator=(const Win32CoverOverlay&) = delete;

  // Starts the layered window at opacity 0; the caller must set opacity
  // afterward. CoverController::FadeIn does this immediately.
  bool Show() override;
  void Hide() override;
  bool SetOpacity(double opacity) override;
  void ReassertTopMost() override;
  bool ConsumeResumeEvent() override;
  bool ConsumeDisplayChange(ScreenMetrics* metrics) override;

  HWND hwnd() const;

 private:
  struct State;

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  static void CALLBACK ForegroundEvent(HWINEVENTHOOK hook, DWORD event,
                                       HWND hwnd, LONG object, LONG child,
                                       DWORD thread, DWORD time);
  static void ThreadMain(std::shared_ptr<State> state);
  static LRESULT HandleMessage(State& state, HWND hwnd, UINT message,
                               WPARAM wparam, LPARAM lparam);
  static void ResizeToVirtualScreen(HWND hwnd);
  static void ApplyTopMost(HWND hwnd);

  std::shared_ptr<State> state_;
  std::thread thread_;
};

} // namespace htmlppt

#endif // _WIN32
