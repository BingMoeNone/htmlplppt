#include "test_main.h"

#include <windows.h>

#include <chrono>

#include "core/clock.h"
#include "platform/win32/pumping_clock.h"

using htmlppt::Duration;
using htmlppt::PumpingClock;

HTMLPPT_TEST_CASE(win32_pumping_clock_drains_thread_messages_while_waiting) {
  PumpingClock clock;
  const UINT message_id = WM_APP + 42;
  HTMLPPT_CHECK(PostThreadMessageW(GetCurrentThreadId(), message_id, 0, 0) !=
                FALSE);
  MSG probe;
  HTMLPPT_CHECK(PeekMessageW(&probe, nullptr, message_id, message_id,
                             PM_NOREMOVE) != FALSE);
  // A plain sleep leaves the message queued; the pumping wait drains it.
  clock.sleep_until(clock.now() + Duration{120});
  HTMLPPT_CHECK(PeekMessageW(&probe, nullptr, message_id, message_id,
                             PM_NOREMOVE) == FALSE);
}

HTMLPPT_TEST_CASE(win32_pumping_clock_waits_at_least_the_deadline) {
  PumpingClock clock;
  const auto start = clock.now();
  clock.sleep_until(start + Duration{80});
  const auto elapsed =
      std::chrono::duration_cast<Duration>(clock.now() - start).count();
  // Timers round down, so allow a few milliseconds of slack below the deadline.
  HTMLPPT_CHECK(elapsed >= 75);
  HTMLPPT_CHECK(elapsed <= 2000);
}

HTMLPPT_TEST_CASE(win32_pumping_clock_returns_immediately_for_a_past_deadline) {
  PumpingClock clock;
  const auto start = clock.now();
  clock.sleep_until(start);
  const auto elapsed =
      std::chrono::duration_cast<Duration>(clock.now() - start).count();
  HTMLPPT_CHECK(elapsed < 50);
}
