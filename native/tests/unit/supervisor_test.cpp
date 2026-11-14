#include "test_main.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "core/supervisor.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::SupervisorAction;
using htmlppt::SupervisorActionKind;
using htmlppt::SupervisorConfig;
using htmlppt::SupervisorCore;
using htmlppt::WorkerEvent;
using htmlppt::WorkerEventKind;

namespace {

bool HasAction(const std::vector<SupervisorAction>& actions,
               SupervisorActionKind kind) {
  return std::any_of(actions.begin(), actions.end(),
                     [kind](const SupervisorAction& action) {
                       return action.kind == kind;
                     });
}

SupervisorConfig Config() {
  SupervisorConfig config;
  config.jitter_ratio = 0.0;
  return config;
}

WorkerEvent Heartbeat(htmlppt::ClockPoint at, std::uint32_t sequence) {
  WorkerEvent event;
  event.kind = WorkerEventKind::Heartbeat;
  event.at = at;
  event.sequence = sequence;
  return event;
}

WorkerEvent Event(WorkerEventKind kind, htmlppt::ClockPoint at) {
  WorkerEvent event;
  event.kind = kind;
  event.at = at;
  return event;
}

void AdvanceToDeadline(SupervisorCore& core, FakeClock& clock) {
  const auto deadline = core.NextDeadline();
  HTMLPPT_CHECK(deadline.has_value());
  clock.advance(std::chrono::duration_cast<Duration>(*deadline -
                                                     clock.now()));
}

} // namespace

HTMLPPT_TEST_CASE(supervisor_start_emits_start_worker) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  const auto actions = core.Start();
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::StartWorker));
  HTMLPPT_CHECK(core.worker_running());
  HTMLPPT_CHECK(!core.safe_mode());
}

HTMLPPT_TEST_CASE(supervisor_heartbeats_keep_worker_healthy) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  core.Handle(Heartbeat(clock.now(), 1));
  clock.advance(Duration{1000});
  core.Handle(Heartbeat(clock.now(), 2));
  clock.advance(Duration{1000});
  HTMLPPT_CHECK(core.Tick().empty());
  HTMLPPT_CHECK(core.worker_running());
}

HTMLPPT_TEST_CASE(supervisor_detects_heartbeat_stall) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  clock.advance(Duration{2001});
  const auto actions = core.Tick();
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RequestDump));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::TerminateWorker));
  HTMLPPT_CHECK(!core.worker_running());
  HTMLPPT_CHECK(core.NextDeadline().has_value());
}

HTMLPPT_TEST_CASE(supervisor_restarts_after_backoff) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  clock.advance(Duration{2001});
  core.Tick();
  AdvanceToDeadline(core, clock);
  const auto actions = core.Tick();
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RestartWorker));
  HTMLPPT_CHECK(core.worker_running());
  HTMLPPT_CHECK_EQ(core.restart_count(), 1);
}

HTMLPPT_TEST_CASE(supervisor_uses_escalating_backoff) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const std::vector<long long> expected{200, 500, 1000, 2000, 5000};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    clock.advance(Duration{2001});
    const auto stall = core.Tick();
    HTMLPPT_CHECK(HasAction(stall, SupervisorActionKind::RequestDump));
    const auto deadline = core.NextDeadline();
    HTMLPPT_CHECK(deadline.has_value());
    HTMLPPT_CHECK_EQ(
        std::chrono::duration_cast<Duration>(*deadline - clock.now()).count(),
        expected[i]);
    AdvanceToDeadline(core, clock);
    const auto restart = core.Tick();
    HTMLPPT_CHECK(HasAction(restart, SupervisorActionKind::RestartWorker));
  }
}

HTMLPPT_TEST_CASE(supervisor_budget_exhaustion_enters_safe_mode) {
  FakeClock clock;
  auto config = Config();
  config.legacy_fallback = true;
  SupervisorCore core(config, clock);
  core.Start();
  for (int i = 0; i < 5; ++i) {
    clock.advance(Duration{2001});
    core.Tick();
    AdvanceToDeadline(core, clock);
    core.Tick();
  }
  clock.advance(Duration{2001});
  const auto actions = core.Tick();
  HTMLPPT_CHECK(core.safe_mode());
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::EnterSafeMode));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RunLegacyFallback));
}

HTMLPPT_TEST_CASE(supervisor_worker_exit_requests_dump_and_restart) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const auto actions = core.Handle(Event(WorkerEventKind::Exited, clock.now()));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RequestDump));
  HTMLPPT_CHECK(!core.worker_running());
  HTMLPPT_CHECK(core.NextDeadline().has_value());
}

HTMLPPT_TEST_CASE(supervisor_hung_event_requests_dump_and_terminate) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const auto actions = core.Handle(Event(WorkerEventKind::Hung, clock.now()));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RequestDump));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::TerminateWorker));
  HTMLPPT_CHECK(!core.worker_running());
}

HTMLPPT_TEST_CASE(supervisor_duplicate_hung_does_not_consume_budget) {
  FakeClock clock;
  auto config = Config();
  config.max_restarts = 1;
  SupervisorCore core(config, clock);
  core.Start();
  core.Handle(Event(WorkerEventKind::Hung, clock.now()));
  const auto duplicate =
      core.Handle(Event(WorkerEventKind::Hung, clock.now()));
  HTMLPPT_CHECK(duplicate.empty());
  HTMLPPT_CHECK(!core.safe_mode());
}

HTMLPPT_TEST_CASE(supervisor_stop_terminates_and_completes) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const auto stop =
      core.Handle(Event(WorkerEventKind::StopRequested, clock.now()));
  HTMLPPT_CHECK(HasAction(stop, SupervisorActionKind::TerminateWorker));
  HTMLPPT_CHECK(core.stopping());
  const auto done = core.Handle(Event(WorkerEventKind::CleanupDone, clock.now()));
  HTMLPPT_CHECK(HasAction(done, SupervisorActionKind::StopComplete));
}

HTMLPPT_TEST_CASE(supervisor_stale_heartbeat_after_stop_is_ignored) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  core.Handle(Event(WorkerEventKind::StopRequested, clock.now()));
  clock.advance(Duration{3000});
  HTMLPPT_CHECK(core.Tick().empty());
}
