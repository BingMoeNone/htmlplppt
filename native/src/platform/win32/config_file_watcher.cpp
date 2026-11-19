#include "platform/win32/config_file_watcher.h"

#ifdef _WIN32

#include <utility>

namespace htmlppt {
namespace {

// A rename (the atomic save) arrives as FILE_NAME, a plain rewrite as
// LAST_WRITE/SIZE; all three must wake the editor.
constexpr DWORD kWatchFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_SIZE;

} // namespace

ConfigFileWatcher::~ConfigFileWatcher() { Stop(); }

bool ConfigFileWatcher::ShouldReload(bool dirty) { return !dirty; }

void ConfigFileWatcher::Start(const std::filesystem::path& directory,
                              HWND target, UINT message) {
  if (running_ || target == nullptr) return;
  const std::filesystem::path folder =
      directory.empty() ? std::filesystem::path(L".") : directory;
  // A notification handle is a simple waitable object: the loop waits on it
  // together with the stop event, so Stop never has to cancel a pending read
  // and cannot race a blocking call.
  change_ = FindFirstChangeNotificationW(folder.c_str(), FALSE, kWatchFilter);
  if (change_ == INVALID_HANDLE_VALUE) {
    change_ = nullptr;
    return;
  }
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_event_ == nullptr) {
    FindCloseChangeNotification(change_);
    change_ = nullptr;
    return;
  }
  target_ = target;
  message_ = message;
  running_ = true;
  thread_ = std::thread([this] { Run(); });
}

void ConfigFileWatcher::Stop() {
  if (!running_ && !thread_.joinable()) return;
  running_ = false;
  // The stop event is manual-reset, so a signal that races the wait is not
  // lost: the next wait returns immediately.
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  if (thread_.joinable()) thread_.join();
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  if (change_ != nullptr) {
    FindCloseChangeNotification(change_);
    change_ = nullptr;
  }
  target_ = nullptr;
  message_ = 0;
}

void ConfigFileWatcher::Run() {
  for (;;) {
    const HANDLE waits[] = {stop_event_, change_};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait != WAIT_OBJECT_0 + 1) break;  // stopped (or a wait failure)
    if (FindNextChangeNotification(change_) == FALSE) break;
    if (target_ != nullptr) PostMessageW(target_, message_, 0, 0);
  }
}

} // namespace htmlppt

#endif // _WIN32
