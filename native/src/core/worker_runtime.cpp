#include "core/worker_runtime.h"

#include <string>
#include <utility>

#include "core/edge_spec.h"
#include "core/focus_model.h"

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
  SyncSnapshot();
}

void WorkerRuntime::Start() {
  Dispatch(machine_.Start());
  SyncSnapshot();
}

bool WorkerRuntime::Step() {
  if (done_) return false;
  ObserveShow();
  ObserveCover();
  ObserveEdge();
  if (pending_stop_.has_value()) {
    std::string reason = std::move(*pending_stop_);
    pending_stop_.reset();
    Handle(MakeStopRequested(ports_.clock.now(), std::move(reason)));
  }
  Dispatch(machine_.Tick());
  SyncSnapshot();
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

void WorkerRuntime::ObserveEdge() {
  if (edge_pid_ == 0) return;
  const ClockPoint now = ports_.clock.now();
  if (!edge_.active()) {
    // The process is gone before or after readiness: clear the whole
    // observation state first so a later step cannot report the same closure
    // twice, matching the LaunchEdge/KillEdge reset.
    edge_pid_ = 0;
    ready_sent_ = false;
    return_sent_ = false;
    Handle(MakeEdgeClosed(now));
    return;
  }
  const std::uint32_t pid = edge_.pid();
  if (!return_sent_) {
    const std::vector<WindowInfo> windows = ports_.windows.Enumerate();
    const auto selected = SelectEdgeWindow(windows, pid);
    if (selected.has_value()) {
      for (const auto& window : windows) {
        if (window.id != *selected) continue;
        if (IsReturnTitle(window.title)) {
          return_sent_ = true;
          Handle(MakeReturnRequested(now));
          return;
        }
        break;
      }
    }
  }
  if (ready_sent_) return;
  // One non-blocking attempt per step: the runtime must keep the poll cadence,
  // so waiting for the Edge window stays the state machine's deadline, not a
  // blocking focus loop.
  if (!focus_.TryFocusEdge(pid)) return;
  ready_sent_ = true;
  Handle(MakeEdgeReady(now));
}

void WorkerRuntime::Handle(const Event& event) {
  Dispatch(machine_.Handle(event));
  SyncSnapshot();
}

void WorkerRuntime::SyncSnapshot() {
  snapshot_ = machine_.Snapshot();
  if (snapshot_.state == State::Faulted && last_state_ != State::Faulted) {
    // Faulted exists so a return can be retried on a fresh ReturnRequested.
    // Re-arm the one-shot signal on entry only: the page keeps the renamed
    // title until Edge closes, so staying Faulted must not re-emit every poll.
    return_sent_ = false;
  }
  last_state_ = snapshot_.state;
}

void WorkerRuntime::Dispatch(const std::vector<Action>& actions) {
  for (const auto& action : actions) Execute(action);
}

void WorkerRuntime::Execute(const Action& action) {
  // An action may block up to its controller/COM timeout: WPS COM has no
  // non-blocking form, and FocusWps keeps the controller's retry loop. The
  // bounded poll interval governs the loop's idle wait at the end of Step(),
  // not an in-flight action.
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
    case ActionKind::LaunchEdge:
      LaunchEdge(action.slide);
      break;
    case ActionKind::KillEdge:
      edge_.Kill();
      // The runtime ended the process itself: stop observing it so the next
      // step cannot report a closure that the runtime already caused.
      edge_pid_ = 0;
      ready_sent_ = false;
      return_sent_ = false;
      break;
    case ActionKind::ExitShow: {
      const HostResult result = ports_.presentation.ExitShow();
      if (!result.ok()) {
        // RetryOperation::ExitShow owns the next attempt and its backoff.
        Handle(MakeFailure(ports_.clock.now(), FailureKind::Transient,
                           FailureSource::Other, "exit show failed"));
      }
      break;
    }
    case ActionKind::ResumeShow:
    case ActionKind::RestoreShow: {
      // ResumeShow ensures the show runs, focuses and jumps; RestoreShow is the
      // same operation after a switch failure. A failure leaves no safe path
      // back into the show, so report it as a return failure and let the
      // machine enter Faulted with its operator diagnostic.
      const HostResult result = ports_.presentation.ResumeShow(action.slide);
      if (!result.ok()) {
        Handle(MakeFailure(ports_.clock.now(), FailureKind::Return,
                           FailureSource::Other, "resume show failed"));
      }
      break;
    }
    case ActionKind::FocusEdge:
      if (edge_pid_ != 0) focus_.TryFocusEdge(edge_.pid());
      break;
    case ActionKind::FocusWps:
      focus_.FocusWps(host_.wps_pids, host_.screen);
      break;
    // Kinds owned by later tasks. They are journaled rather than ignored so a
    // missing case stays visible instead of vanishing in a default branch.
    case ActionKind::EnterSafeMode:
    case ActionKind::LegacyFallback:
    case ActionKind::StopHelperThreads:
    case ActionKind::ReleaseLock:
      journal_.push_back(action.detail);
      break;
  }
}

void WorkerRuntime::LaunchEdge(int slide) {
  const ResolvedTarget* target = nullptr;
  for (const auto& candidate : plan_.targets) {
    if (candidate.slide == slide) {
      target = &candidate;
      break;
    }
  }
  if (target == nullptr) {
    ReportEdgeLaunchFailure("edge launch failed: no target for slide " +
                            std::to_string(slide));
    return;
  }
  const EdgeLaunchSpec spec =
      MakeEdgeLaunchSpec(host_.edge_exe, host_.temp_dir, *target, host_.edge);
  if (!edge_.Launch(spec)) {
    // No pid to observe: the machine's retry policy owns the next attempt.
    edge_pid_ = 0;
    ready_sent_ = false;
    return_sent_ = false;
    ReportEdgeLaunchFailure("edge launch failed");
    return;
  }
  edge_pid_ = edge_.pid();
  ready_sent_ = false;
  return_sent_ = false;
}

void WorkerRuntime::ReportEdgeLaunchFailure(std::string detail) {
  journal_.push_back(detail);
  Handle(MakeFailure(ports_.clock.now(), FailureKind::Transient,
                     FailureSource::Edge, std::move(detail)));
}

} // namespace htmlppt
