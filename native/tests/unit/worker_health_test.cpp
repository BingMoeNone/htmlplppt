#include "test_main.h"

#include <algorithm>

#include "core/worker_health.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::ThreadRole;
using htmlppt::WorkerHealthMonitor;

namespace {

bool Contains(const std::vector<ThreadRole>& roles, ThreadRole role) {
  return std::find(roles.begin(), roles.end(), role) != roles.end();
}

} // namespace

HTMLPPT_TEST_CASE(worker_health_registered_thread_is_healthy) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::ComState, clock.now());
  HTMLPPT_CHECK(monitor.Healthy(clock.now(), Duration{1000}));
  HTMLPPT_CHECK_EQ(monitor.registered(), std::size_t{1});
}

HTMLPPT_TEST_CASE(worker_health_detects_stale_thread) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::ComState, clock.now());
  clock.advance(Duration{1000});
  HTMLPPT_CHECK(monitor.Healthy(clock.now(), Duration{1000}));
  clock.advance(Duration{1});
  const auto stale = monitor.Stale(clock.now(), Duration{1000});
  HTMLPPT_CHECK_EQ(stale.size(), std::size_t{1});
  HTMLPPT_CHECK(Contains(stale, ThreadRole::ComState));
}

HTMLPPT_TEST_CASE(worker_health_beat_refreshes_thread) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::Focus, clock.now());
  clock.advance(Duration{900});
  monitor.Beat(ThreadRole::Focus, clock.now());
  clock.advance(Duration{900});
  HTMLPPT_CHECK(monitor.Healthy(clock.now(), Duration{1000}));
}

HTMLPPT_TEST_CASE(worker_health_reports_only_stale_roles) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::ComState, clock.now());
  monitor.Register(ThreadRole::Focus, clock.now());
  clock.advance(Duration{500});
  monitor.Beat(ThreadRole::Focus, clock.now());
  clock.advance(Duration{501});
  const auto stale = monitor.Stale(clock.now(), Duration{1000});
  HTMLPPT_CHECK_EQ(stale.size(), std::size_t{1});
  HTMLPPT_CHECK(Contains(stale, ThreadRole::ComState));
}

HTMLPPT_TEST_CASE(worker_health_unknown_beat_is_ignored) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Beat(ThreadRole::UiOverlay, clock.now());
  HTMLPPT_CHECK(!monitor.LastBeat(ThreadRole::UiOverlay).has_value());
  HTMLPPT_CHECK_EQ(monitor.registered(), std::size_t{0});
}
