#pragma once

#ifdef _WIN32

#include <windows.h>

#include <filesystem>
#include <string>
#include <thread>

namespace htmlppt {

// Watches one config file's directory for on-disk changes and posts a private
// window message, so the editor's UI thread can reload the model or report a
// conflict. The wait runs on its own thread; the UI thread only receives the
// notification.
//
// The loop waits on a change-notification handle plus a stop event, so Stop()
// only has to signal the event and join: no overlapped read needs cancelling.
class ConfigFileWatcher {
 public:
  ConfigFileWatcher() = default;
  ConfigFileWatcher(const ConfigFileWatcher&) = delete;
  ConfigFileWatcher& operator=(const ConfigFileWatcher&) = delete;
  ~ConfigFileWatcher();

  // The reload policy, pure so it is testable without the filesystem: an
  // external change replaces the model only when the editor has no unsaved
  // edits; a dirty editor keeps its edits and reports the conflict instead.
  static bool ShouldReload(bool dirty);

  // Watches `directory` for changes and posts `message` to `target` for each
  // one. A missing directory or a failed thread start leaves the watcher idle
  // (running() stays false); the editor keeps working without live notices.
  void Start(const std::filesystem::path& directory, HWND target, UINT message);
  // Idempotent: signals the loop, joins the thread, and closes the handles.
  void Stop();
  bool running() const { return running_; }

 private:
  void Run();

  HANDLE change_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HWND target_ = nullptr;
  UINT message_ = 0;
  std::thread thread_;
  bool running_ = false;
};

} // namespace htmlppt

#endif // _WIN32
