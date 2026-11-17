#pragma once

#ifdef _WIN32

#include <windows.h>

#include <string>

namespace htmlppt {

struct ConfigWindowOptions {
  std::wstring title;
  int width = 900;
  int height = 560;
  // Tests create the window hidden so a headless run never flashes a window.
  bool show = true;
};

// The config editor's top-level window. The view is deliberately thin: it owns
// the window and its controls, and all edits go through ConfigEditorModel.
class ConfigWindow {
 public:
  ConfigWindow() = default;
  ConfigWindow(const ConfigWindow&) = delete;
  ConfigWindow& operator=(const ConfigWindow&) = delete;
  ~ConfigWindow();

  bool Create(const ConfigWindowOptions& options);
  // Runs until WM_QUIT; returns the quit code.
  int RunMessageLoop();
  // Posts WM_CLOSE (the window may refuse while dirty; see Task 8).
  bool Close();
  HWND handle() const { return handle_; }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  HWND handle_ = nullptr;
};

} // namespace htmlppt

#endif // _WIN32
