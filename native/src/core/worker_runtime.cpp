#include "core/worker_runtime.h"

#include <utility>

namespace htmlppt {

WorkerRuntime::WorkerRuntime(WorkerPlan plan, StateMachineConfig config,
                             WorkerPorts ports, WorkerHostConfig host)
    : plan_(std::move(plan)),
      host_(std::move(host)),
      ports_(ports),
      cover_(host_.cover, ports_.clock, ports_.overlay),
      edge_(ports_.launcher),
      focus_(ports_.windows, ports_.clock, host_.focus),
      machine_(plan_.machine, std::move(config), ports_.clock) {
  snapshot_ = machine_.Snapshot();
}

void WorkerRuntime::Start() {
  Dispatch(machine_.Start());
  snapshot_ = machine_.Snapshot();
}

bool WorkerRuntime::Step() {
  if (done_) return false;
  ObserveShow();
  ObserveCover();
  if (pending_stop_.has_value()) {
    std::string reason = std::move(*pending_stop_);
    pending_stop_.reset();
    Handle(MakeStopRequested(ports_.clock.now(), std::move(reason)));
  }
  Dispatch(machine_.Tick());
  snapshot_ = machine_.Snapshot();
  done_ =
      machine_.state() == State::Exited || machine_.state() == State::SafeMode;
  if (done_) return false;
  ports_.clock.sleep_until(ports_.clock.now() + host_.poll_interval);
  return true;
}

void WorkerRuntime::RequestStop(std::string reason) {
  pending_stop_ = std::move(reason);
}

void WorkerRuntime::ObserveShow() {
  const ShowState show = ports_.presentation.QueryShow();
  const ClockPoint now = ports_.clock.now();
  if (show.status == HostStatus::Busy) {
    // A busy COM query is not a show state: keep the last good sample so the
    // busy poll cannot look like a slide change or a show end.
    Handle(MakeFailure(now, FailureKind::Transient, FailureSource::Com,
                       show.detail.empty() ? "COM busy" : show.detail));
    return;
  }
  if (show.active != last_show_.active) {
    Handle(show.active ? MakeShowStarted(now) : MakeShowEnded(now));
  }
  if (show.active && show.slide > 0 && show.slide != last_slide_) {
    Handle(MakeSlideChanged(now, last_slide_, show.slide));
    last_slide_ = show.slide;
  }
  last_show_ = show;
}

void WorkerRuntime::ObserveCover() {
  const CoverTick tick = cover_.Tick();
  const ClockPoint now = ports_.clock.now();
  if (tick.hold_reached && !last_cover_hold_) {
    Handle(MakeCoverHold(now));
  }
  if (tick.done && !last_cover_done_) {
    Handle(MakeCoverDone(now));
  }
  last_cover_hold_ = tick.hold_reached;
  last_cover_done_ = tick.done;
}

void WorkerRuntime::Handle(const Event& event) {
  Dispatch(machine_.Handle(event));
  snapshot_ = machine_.Snapshot();
}

void WorkerRuntime::Dispatch(const std::vector<Action>& actions) {
  for (const auto& action : actions) Execute(action);
}

void WorkerRuntime::Execute(const Action& action) {
  switch (action.kind) {
    case ActionKind::CoverFadeIn:
      cover_.FadeIn();
      break;
    case ActionKind::CoverFadeOut:
      cover_.FadeOut();
      break;
    case ActionKind::CoverHide:
      cover_.Hide();
      break;
    case ActionKind::Journal:
    case ActionKind::Diagnostic:
      journal_.push_back(action.detail);
      break;
    // Kinds owned by later tasks. They are journaled rather than ignored so a
    // missing case stays visible instead of vanishing in a default branch.
    case ActionKind::LaunchEdge:
    case ActionKind::KillEdge:
    case ActionKind::ExitShow:
    case ActionKind::ResumeShow:
    case ActionKind::RestoreShow:
    case ActionKind::FocusEdge:
    case ActionKind::FocusWps:
    case ActionKind::EnterSafeMode:
    case ActionKind::LegacyFallback:
    case ActionKind::StopHelperThreads:
    case ActionKind::ReleaseLock:
      journal_.push_back(action.detail);
      break;
  }
}

} // namespace htmlppt
