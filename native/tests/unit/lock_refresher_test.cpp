#include "test_main.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "core/lock_refresher.h"

using htmlppt::Duration;
using htmlppt::LockRefresher;

// The legacy watcher treats a lock older than 15 s as stale. The refresh must
// run on its own timer so a blocking WriteLock can only delay the refresher,
// never the worker's heartbeat thread.
HTMLPPT_TEST_CASE(lock_refresher_repeats_on_its_own_timer) {
  std::atomic<int> calls{0};
  {
    LockRefresher refresher([&calls] {
      calls.fetch_add(1, std::memory_order_relaxed);
      return true;
    }, Duration{10});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (calls.load(std::memory_order_relaxed) < 2 &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  HTMLPPT_CHECK(calls.load(std::memory_order_relaxed) >= 2);
  // The destructor joins: no timer callback may run after it returns.
  const int after_stop = calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  HTMLPPT_CHECK_EQ(calls.load(std::memory_order_relaxed), after_stop);
}
