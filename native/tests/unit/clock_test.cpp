#include "test_main.h"

#include <chrono>
#include <cstddef>

#include "core/clock.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::SteadyClock;

HTMLPPT_TEST_CASE(fake_clock_advances) {
  FakeClock clock;
  const auto start = clock.now();
  clock.advance(Duration{250});
  HTMLPPT_CHECK_EQ(
      std::chrono::duration_cast<Duration>(clock.now() - start).count(), 250);
}

HTMLPPT_TEST_CASE(fake_clock_records_sleeps) {
  FakeClock clock;
  const auto start = clock.now();
  clock.sleep_until(start + Duration{100});
  HTMLPPT_CHECK_EQ(clock.sleeps().size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(
      std::chrono::duration_cast<Duration>(clock.now() - start).count(), 100);
}

HTMLPPT_TEST_CASE(steady_clock_is_non_decreasing) {
  SteadyClock clock;
  const auto first = clock.now();
  const auto second = clock.now();
  HTMLPPT_CHECK(second >= first);
}
