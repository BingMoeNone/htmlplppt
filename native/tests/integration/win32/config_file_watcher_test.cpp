#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "platform/win32/config_file_watcher.h"
#include "platform/win32/config_window.h"
#include "test_utils.h"

// The reload policy is the testable half of the watcher: an external change
// replaces the model only when the editor has nothing unsaved. A dirty editor
// keeps its edits and the app reports the conflict instead.
HTMLPPT_TEST_CASE(win32_config_file_watcher_reloads_only_when_clean) {
  HTMLPPT_CHECK(htmlppt::ConfigFileWatcher::ShouldReload(/*dirty=*/false));
  HTMLPPT_CHECK(!htmlppt::ConfigFileWatcher::ShouldReload(/*dirty=*/true));
}

// The notification loop is manual (a real ReadDirectoryChangesW wake-up is
// timing dependent), but its lifecycle is not: starting on a real directory
// owns a thread and handles, and stopping joins it and is safe to repeat.
HTMLPPT_TEST_CASE(win32_config_file_watcher_starts_and_stops) {
  htmlppt::test::TempDir directory;
  const std::filesystem::path config = directory.path() / L"watcher.json";
  std::ofstream(config) << "{}";

  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));

  {
    htmlppt::ConfigFileWatcher watcher;
    HTMLPPT_CHECK(!watcher.running());
    watcher.Start(directory.path(), window.handle(),
                  htmlppt::kConfigChangedMessage);
    HTMLPPT_CHECK(watcher.running());
    watcher.Stop();
    HTMLPPT_CHECK(!watcher.running());
    // Stop is idempotent; a second call must not crash or close a freed handle.
    watcher.Stop();
    HTMLPPT_CHECK(!watcher.running());
  }

  const HWND handle = window.handle();
  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(handle) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(handle) == 0);
}

// A real on-disk change must reach the window as kConfigChangedMessage: the
// loop waits on the notification handle, so this also proves the wait is not
// stuck behind the stop event.
HTMLPPT_TEST_CASE(win32_config_file_watcher_posts_a_change_notification) {
  htmlppt::test::TempDir directory;
  const std::filesystem::path config = directory.path() / L"watcher.json";
  std::ofstream(config) << "{}";

  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  int notifications = 0;
  window.SetConfigChangedHandler([&notifications] { ++notifications; });

  htmlppt::ConfigFileWatcher watcher;
  watcher.Start(directory.path(), window.handle(),
                htmlppt::kConfigChangedMessage);
  HTMLPPT_CHECK(watcher.running());

  {
    std::ofstream append(config, std::ios::app);
    append << " ";
  }
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && notifications == 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  watcher.Stop();
  HTMLPPT_CHECK(notifications > 0);

  const HWND handle = window.handle();
  window.Close();
  const DWORD close_deadline = GetTickCount() + 5000;
  while (GetTickCount() < close_deadline && IsWindow(handle) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(handle) == 0);
}
#endif // _WIN32
