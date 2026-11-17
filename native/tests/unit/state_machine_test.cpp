#include "test_main.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/state_machine.h"

using htmlppt::Action;
using htmlppt::ActionKind;
using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::FailureKind;
using htmlppt::FailureSource;
using htmlppt::MakeCleanupDone;
using htmlppt::MakeComHealthy;
using htmlppt::MakeCoverDone;
using htmlppt::MakeCoverHold;
using htmlppt::MakeEdgeClosed;
using htmlppt::MakeEdgeReady;
using htmlppt::MakeFailure;
using htmlppt::MakeReturnRequested;
using htmlppt::MakeShowEnded;
using htmlppt::MakeShowStarted;
using htmlppt::MakeSlideChanged;
using htmlppt::MakeStopRequested;
using htmlppt::MakeSupervisorLost;
using htmlppt::OnFailurePolicy;
using htmlppt::RunPlan;
using htmlppt::State;
using htmlppt::StateMachine;
using htmlppt::StateMachineConfig;
using htmlppt::TriggerMode;

namespace {

bool HasAction(const std::vector<Action>& actions, ActionKind kind) {
  return std::any_of(actions.begin(), actions.end(),
                     [kind](const Action& action) {
                       return action.kind == kind;
                     });
}

const Action* FindAction(const std::vector<Action>& actions, ActionKind kind) {
  for (const auto& action : actions) {
    if (action.kind == kind) return &action;
  }
  return nullptr;
}

bool HasJournalContaining(const std::vector<Action>& actions,
                          std::string_view text) {
  return std::any_of(actions.begin(), actions.end(),
                     [text](const Action& action) {
                       return action.kind == ActionKind::Journal &&
                              action.detail.find(text) != std::string::npos;
                     });
}

RunPlan MakePlan() {
  RunPlan plan;
  plan.run_id = "test-run";
  plan.slide_count = 10;
  plan.targets = {
      {.slide = 5,
       .label = "Audit",
       .edge_profile = "tef-live-audit",
       .max_consecutive_trigger_failures = std::nullopt,
       .cover_max_hold = std::nullopt},
      {.slide = 8,
       .label = "Close",
       .edge_profile = "tef-live-close",
       .max_consecutive_trigger_failures = std::nullopt,
       .cover_max_hold = std::nullopt},
  };
  return plan;
}

StateMachineConfig MakeConfig() {
  StateMachineConfig config;
  config.startup_ignore = Duration{0};
  return config;
}

struct Machine {
  FakeClock clock;
  RunPlan plan;
  StateMachineConfig config;
  std::unique_ptr<StateMachine> machine;

  explicit Machine(StateMachineConfig config_in = MakeConfig(),
                   RunPlan plan_in = MakePlan())
      : plan(std::move(plan_in)), config(std::move(config_in)) {
    machine = std::make_unique<StateMachine>(plan, config, clock);
    machine->Start();
    machine->Handle(MakeShowStarted(clock.now()));
  }

  std::vector<Action> Slide(int old_slide, int new_slide) {
    return machine->Handle(MakeSlideChanged(clock.now(), old_slide, new_slide));
  }

  std::vector<Action> TriggerTargetFive() {
    Slide(4, 5);
    return Slide(5, 6);
  }

  void Advance(int ms) { clock.advance(Duration{ms}); }

  void AdvanceToNextDeadline() {
    const auto deadline = machine->NextDeadline();
    HTMLPPT_CHECK(deadline.has_value());
    clock.advance(std::chrono::duration_cast<Duration>(*deadline -
                                                       clock.now()));
  }

  void ReachLive() {
    TriggerTargetFive();
    machine->Handle(MakeCoverHold(clock.now()));
    machine->Handle(MakeEdgeReady(clock.now()));
    machine->Handle(MakeShowEnded(clock.now()));
  }

  void ReturnToMonitoring() {
    machine->Handle(MakeReturnRequested(clock.now()));
    machine->Handle(MakeCoverHold(clock.now()));
    machine->Handle(MakeCoverDone(clock.now()));
  }
};

} // namespace

HTMLPPT_TEST_CASE(state_machine_starts_in_warming_and_show_started_enters_monitoring) {
  FakeClock clock;
  StateMachine machine(MakePlan(), MakeConfig(), clock);
  HTMLPPT_CHECK(machine.state() == State::Boot);
  const auto started = machine.Start();
  HTMLPPT_CHECK(machine.state() == State::Warming);
  HTMLPPT_CHECK(HasJournalContaining(started, "state=Warming"));
  const auto shown = machine.Handle(MakeShowStarted(clock.now()));
  HTMLPPT_CHECK(machine.state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(shown, ActionKind::FocusWps));
  HTMLPPT_CHECK(machine.Snapshot().show_active);
}

HTMLPPT_TEST_CASE(state_machine_startup_ignore_window_suppresses_trigger) {
  FakeClock clock;
  auto config = MakeConfig();
  config.startup_ignore = Duration{1000};
  StateMachine machine(MakePlan(), config, clock);
  machine.Start();
  machine.Handle(MakeShowStarted(clock.now()));
  clock.advance(Duration{500});
  auto ignored = machine.Handle(MakeSlideChanged(clock.now(), 4, 5));
  HTMLPPT_CHECK(!HasAction(ignored, ActionKind::CoverFadeIn));
  clock.advance(Duration{400});
  ignored = machine.Handle(MakeSlideChanged(clock.now(), 5, 6));
  HTMLPPT_CHECK(!HasAction(ignored, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(machine.state() == State::Monitoring);
  clock.advance(Duration{200});
  machine.Handle(MakeSlideChanged(clock.now(), 4, 5));
  const auto triggered = machine.Handle(MakeSlideChanged(clock.now(), 5, 6));
  HTMLPPT_CHECK(HasAction(triggered, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(machine.state() == State::CoverIn);
}

HTMLPPT_TEST_CASE(state_machine_forward_leave_from_target_triggers) {
  Machine m;
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(m.machine->state() == State::CoverIn);
  HTMLPPT_CHECK(HasJournalContaining(actions, "trigger slide 5 -> Audit"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
  const auto snapshot = m.machine->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.current_target_slide.value_or(0), 5);
  HTMLPPT_CHECK(m.machine->IsDone(5));
  HTMLPPT_CHECK_EQ(snapshot.trigger_generation, std::uint64_t{1});
}

HTMLPPT_TEST_CASE(state_machine_backward_leave_does_not_trigger) {
  Machine m;
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 4);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
}

HTMLPPT_TEST_CASE(state_machine_arrive_trigger_fires_on_entry) {
  auto plan = MakePlan();
  plan.targets[0].trigger = TriggerMode::Arrive;
  Machine m(MakeConfig(), plan);
  const auto actions = m.Slide(4, 5);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->state() == State::CoverIn);
}

HTMLPPT_TEST_CASE(state_machine_cover_hold_launches_edge) {
  Machine m;
  m.TriggerTargetFive();
  const auto actions = m.machine->Handle(MakeCoverHold(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::EdgeStarting);
  const auto* launch = FindAction(actions, ActionKind::LaunchEdge);
  HTMLPPT_CHECK(launch != nullptr);
  HTMLPPT_CHECK_EQ(launch->slide, 5);
  HTMLPPT_CHECK_EQ(launch->detail, std::string{"tef-live-audit"});
}

HTMLPPT_TEST_CASE(state_machine_edge_ready_exits_show) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto actions = m.machine->Handle(MakeEdgeReady(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::ShowExit);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::ExitShow));
}

HTMLPPT_TEST_CASE(state_machine_show_ended_enters_live_and_sets_deadline) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  const auto actions = m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Live);
  HTMLPPT_CHECK(HasJournalContaining(actions, "live"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeOut));
  const auto deadline = m.machine->NextDeadline();
  HTMLPPT_CHECK(deadline.has_value());
  HTMLPPT_CHECK_EQ(
      std::chrono::duration_cast<Duration>(*deadline - m.clock.now()).count(),
      600000);
}

HTMLPPT_TEST_CASE(state_machine_return_requested_fades_and_resumes) {
  Machine m;
  m.ReachLive();
  const auto actions = m.machine->Handle(MakeReturnRequested(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasJournalContaining(actions, "return requested by page"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->Snapshot().return_prepared);
}

HTMLPPT_TEST_CASE(state_machine_cover_hold_in_returning_resumes_show) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto actions = m.machine->Handle(MakeCoverHold(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::ReturnCoverOut);
  const auto* resume = FindAction(actions, ActionKind::ResumeShow);
  HTMLPPT_CHECK(resume != nullptr);
  HTMLPPT_CHECK_EQ(resume->slide, 5);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::FocusWps));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeOut));
  HTMLPPT_CHECK(HasJournalContaining(actions, "resumed at slide 5"));
  std::size_t resume_index = actions.size();
  std::size_t fade_index = actions.size();
  for (std::size_t i = 0; i < actions.size(); ++i) {
    if (actions[i].kind == ActionKind::ResumeShow) resume_index = i;
    if (actions[i].kind == ActionKind::CoverFadeOut) fade_index = i;
  }
  HTMLPPT_CHECK(resume_index < fade_index);
}

HTMLPPT_TEST_CASE(state_machine_cover_done_returns_to_monitoring) {
  Machine m;
  m.ReachLive();
  m.ReturnToMonitoring();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  const auto snapshot = m.machine->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.prev_slide, -1);
  HTMLPPT_CHECK(!snapshot.current_target_slide.has_value());
  HTMLPPT_CHECK(snapshot.ignore_until.has_value());
  HTMLPPT_CHECK(!snapshot.return_prepared);
}

HTMLPPT_TEST_CASE(state_machine_edge_closed_without_signal_returns) {
  Machine m;
  m.ReachLive();
  const auto actions = m.machine->Handle(MakeEdgeClosed(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasJournalContaining(actions,
                                     "edge closed after return; cover fade out"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_edge_live_timeout_kills_edge_and_returns) {
  Machine m;
  m.ReachLive();
  m.Advance(600001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_self_heals) {
  Machine m;
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::KillEdge));
  const auto* restore = FindAction(actions, ActionKind::RestoreShow);
  HTMLPPT_CHECK(restore != nullptr);
  HTMLPPT_CHECK_EQ(restore->slide, 5);
  HTMLPPT_CHECK(!m.machine->IsDone(5));
  HTMLPPT_CHECK_EQ(m.machine->Snapshot().consecutive_trigger_failures, 1);
  m.Advance(2001);
  m.Slide(4, 5);
  const auto retrigger = m.Slide(5, 6);
  HTMLPPT_CHECK(HasAction(retrigger, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_strict_keeps_done) {
  auto plan = MakePlan();
  plan.targets[0].on_failure = OnFailurePolicy::Strict;
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(m.machine->IsDone(5));
  m.Advance(2001);
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_safe_enters_safe_mode) {
  auto plan = MakePlan();
  plan.targets[0].on_failure = OnFailurePolicy::Safe;
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_legacy_emits_fallback) {
  auto plan = MakePlan();
  plan.targets[0].on_failure = OnFailurePolicy::Legacy;
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::LegacyFallback));
}

HTMLPPT_TEST_CASE(state_machine_consecutive_switch_failures_enter_safe_mode) {
  auto config = MakeConfig();
  config.max_consecutive_trigger_failures = 2;
  Machine m(config);
  m.TriggerTargetFive();
  m.Advance(10001);
  m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  m.Advance(2001);
  m.Slide(4, 5);
  m.Slide(5, 6);
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_edge_ready_timeout_is_switch_failure) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.Advance(20001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_transient_failure_retries_after_backoff) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  const auto failed = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "exit failed"));
  HTMLPPT_CHECK(HasJournalContaining(failed, "retry in"));
  HTMLPPT_CHECK(m.machine->NextDeadline().has_value());
  m.AdvanceToNextDeadline();
  const auto retried = m.machine->Tick();
  HTMLPPT_CHECK(HasAction(retried, ActionKind::ExitShow));
  m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Live);
}

HTMLPPT_TEST_CASE(state_machine_launch_retry_kills_edge_first) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto failed = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Edge,
                  "launch failed"));
  HTMLPPT_CHECK(HasJournalContaining(failed, "retry in"));
  m.AdvanceToNextDeadline();
  const auto retried = m.machine->Tick();
  HTMLPPT_CHECK(HasAction(retried, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(retried, ActionKind::LaunchEdge));
  std::size_t kill_index = retried.size();
  std::size_t launch_index = retried.size();
  for (std::size_t i = 0; i < retried.size(); ++i) {
    if (retried[i].kind == ActionKind::KillEdge) kill_index = i;
    if (retried[i].kind == ActionKind::LaunchEdge) launch_index = i;
  }
  HTMLPPT_CHECK(kill_index < launch_index);
}

HTMLPPT_TEST_CASE(state_machine_retries_exhausted_escalate_to_switch_failure) {
  auto config = MakeConfig();
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Transient,
                                FailureSource::Other, "exit failed"));
  m.AdvanceToNextDeadline();
  m.machine->Tick();
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "exit failed again"));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_com_error_window_escalates_to_structural) {
  auto config = MakeConfig();
  config.com_error_window = Duration{2000};
  config.retry.max_attempts = 10;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto first = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(!HasAction(first, ActionKind::EnterSafeMode));
  m.Advance(2000);
  const auto second = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(second, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_return_failure_enters_faulted_and_keeps_edge) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Return, FailureSource::Other,
                  "resume failed"));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::KillEdge));
  HTMLPPT_CHECK(m.machine->Snapshot().return_prepared);
}

HTMLPPT_TEST_CASE(state_machine_faulted_edge_closed_only_journals) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Return,
                                FailureSource::Other, "resume failed"));
  const auto actions = m.machine->Handle(MakeEdgeClosed(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Journal));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_faulted_return_request_retries_return) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Return,
                                FailureSource::Other, "resume failed"));
  const auto actions = m.machine->Handle(MakeReturnRequested(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_structural_failure_enters_safe_mode) {
  Machine m;
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Structural, FailureSource::Other,
                  "config missing"));
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_stop_requested_cleans_up_and_exits) {
  Machine m;
  m.ReachLive();
  const auto stopping =
      m.machine->Handle(MakeStopRequested(m.clock.now(), "test"));
  HTMLPPT_CHECK(m.machine->state() == State::Stopping);
  HTMLPPT_CHECK(HasJournalContaining(stopping, "cleanup start"));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::StopHelperThreads));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::ReleaseLock));
  const auto done = m.machine->Handle(MakeCleanupDone(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Exited);
  HTMLPPT_CHECK(HasJournalContaining(done, "cleanup done"));
}

HTMLPPT_TEST_CASE(state_machine_supervisor_loss_continue_sets_degraded) {
  Machine m;
  m.ReachLive();
  const auto actions = m.machine->Handle(
      MakeSupervisorLost(m.clock.now(), "pipe closed"));
  HTMLPPT_CHECK(m.machine->state() == State::Live);
  HTMLPPT_CHECK(m.machine->Snapshot().degraded_no_supervisor);
  HTMLPPT_CHECK(HasJournalContaining(actions, "supervisor lost"));
}

HTMLPPT_TEST_CASE(state_machine_supervisor_loss_cleanup_exits) {
  auto config = MakeConfig();
  config.supervisor_loss = htmlppt::SupervisorLossPolicy::Cleanup;
  Machine m(config);
  m.ReachLive();
  const auto actions = m.machine->Handle(
      MakeSupervisorLost(m.clock.now(), "pipe closed"));
  HTMLPPT_CHECK(m.machine->state() == State::Stopping);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  m.machine->Handle(MakeCleanupDone(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Exited);
}

HTMLPPT_TEST_CASE(state_machine_cover_done_timeout_hides_cover_and_monitors) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
  const auto* hide = FindAction(actions, ActionKind::CoverHide);
  HTMLPPT_CHECK(hide != nullptr);
  HTMLPPT_CHECK_EQ(hide->slide, 5);
}

HTMLPPT_TEST_CASE(state_machine_show_ended_resets_done_flags) {
  Machine m;
  m.ReachLive();
  m.ReturnToMonitoring();
  m.Advance(2001);
  HTMLPPT_CHECK(m.machine->IsDone(5));
  m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(!m.machine->IsDone(5));
  m.machine->Handle(MakeShowStarted(m.clock.now()));
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_once_per_show) {
  Machine m;
  m.ReachLive();
  m.ReturnToMonitoring();
  m.Advance(2001);
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
}

HTMLPPT_TEST_CASE(state_machine_snapshot_reports_guards) {
  Machine m;
  m.TriggerTargetFive();
  const auto snapshot = m.machine->Snapshot();
  HTMLPPT_CHECK(snapshot.state == State::CoverIn);
  HTMLPPT_CHECK_EQ(snapshot.prev_slide, 6);
  HTMLPPT_CHECK_EQ(snapshot.current_target_slide.value_or(0), 5);
  HTMLPPT_CHECK(snapshot.show_active);
  HTMLPPT_CHECK(m.machine->IsDone(5));
}

HTMLPPT_TEST_CASE(state_machine_return_resume_timeout_enters_faulted) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::KillEdge));
}

HTMLPPT_TEST_CASE(state_machine_returning_transient_failure_retries_then_faults) {
  auto config = MakeConfig();
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  Machine m(config);
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto failed = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "cover failed"));
  HTMLPPT_CHECK(HasJournalContaining(failed, "retry in"));
  m.AdvanceToNextDeadline();
  const auto retried = m.machine->Tick();
  HTMLPPT_CHECK(HasAction(retried, ActionKind::CoverFadeIn));
  const auto exhausted = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "cover failed again"));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(exhausted, ActionKind::CoverHide));
}

HTMLPPT_TEST_CASE(state_machine_show_ended_during_return_enters_faulted) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto actions = m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
}

HTMLPPT_TEST_CASE(state_machine_edge_closed_before_ready_is_switch_failure) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto actions = m.machine->Handle(MakeEdgeClosed(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_com_success_resets_window) {
  auto config = MakeConfig();
  config.com_error_window = Duration{2000};
  config.retry.max_attempts = 10;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Transient,
                                FailureSource::Com, "COM busy"));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  m.Advance(2000);
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(m.machine->state() != State::SafeMode);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::EnterSafeMode));
}

// Spec 3.6: ComHealthy is the machine's explicit "reset the window" signal, so
// a healthy poll after a run of COM failures must start the window over.
HTMLPPT_TEST_CASE(state_machine_com_healthy_resets_error_window) {
  auto config = MakeConfig();
  config.com_error_window = Duration{2000};
  config.retry.max_attempts = 10;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Transient,
                                FailureSource::Com, "COM busy"));
  const auto healthy = m.machine->Handle(MakeComHealthy(m.clock.now()));
  HTMLPPT_CHECK(healthy.empty());
  m.Advance(2000);
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(m.machine->state() != State::SafeMode);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_per_target_cover_max_hold_override) {
  auto plan = MakePlan();
  plan.targets[0].cover_max_hold = Duration{5000};
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(5001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_per_target_failure_threshold_override) {
  auto config = MakeConfig();
  config.max_consecutive_trigger_failures = 5;
  auto plan = MakePlan();
  plan.targets[0].max_consecutive_trigger_failures = 1;
  Machine m(config, plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_events_after_exit_are_ignored) {
  Machine m;
  m.machine->Handle(MakeStopRequested(m.clock.now(), "test"));
  m.machine->Handle(MakeCleanupDone(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Exited);
  HTMLPPT_CHECK(m.Slide(1, 2).empty());
  HTMLPPT_CHECK(m.machine->Handle(MakeShowStarted(m.clock.now())).empty());
}
