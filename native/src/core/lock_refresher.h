#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "core/clock.h"

namespace htmlppt {

// Refreshes a periodically-rewritten resource (the legacy watcher.lock) on its
// own timer thread. The heartbeat callback must not block, so the file write
// lives here instead of in WorkerSessionConfig::on_heartbeat.
class LockRefresher {
 public:
  using Writer = std::function<bool()>;

  LockRefresher(Writer writer, Duration interval);
  ~LockRefresher();

  LockRefresher(const LockRefresher&) = delete;
  LockRefresher& operator=(const LockRefresher&) = delete;

  // Idempotent; the destructor calls it.
  void Stop();

 private:
  void Refresh();
  void Run();

  Writer writer_;
  Duration interval_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

} // namespace htmlppt
