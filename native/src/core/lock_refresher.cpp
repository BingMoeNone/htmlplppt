#include "core/lock_refresher.h"

#include <utility>

namespace htmlppt {

LockRefresher::LockRefresher(Writer writer, Duration interval)
    : writer_(std::move(writer)), interval_(interval) {
  if (writer_ && interval_.count() > 0) {
    // Refresh once before the first interval: the guard may start this after a
    // long startup, and the lock must not already look stale.
    Refresh();
    thread_ = std::thread(&LockRefresher::Run, this);
  }
}

LockRefresher::~LockRefresher() { Stop(); }

void LockRefresher::Refresh() {
  try {
    writer_();
  } catch (...) {
    // A failed refresh is retried on the next interval; it must not kill the
    // worker or the refresher.
  }
}

void LockRefresher::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void LockRefresher::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    if (wake_.wait_for(lock, interval_, [this] { return stopping_; })) break;
    lock.unlock();
    Refresh();
    lock.lock();
  }
}

} // namespace htmlppt
