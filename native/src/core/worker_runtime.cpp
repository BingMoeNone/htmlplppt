#include "core/worker_runtime.h"

#include <cstddef>
#include <string>
#include <utility>

#include "core/edge_spec.h"
#include "core/focus_model.h"
#include "core/staging.h"
#include "core/utf8_path.h"

namespace htmlppt {
namespace {

// The journal is diagnostic: a poll loop that keeps failing must not grow it
// without bound, so it keeps the most recent entries only.
constexpr std::size_t kMaxJournalEntries = 500;

// Names a failed query in the journal. The caller keeps the last good show
// sample: a failed query is not a show state, so it can never be read as a show
// end.
std::string ComFailureDetail(const ShowState& show) {
  if (!show.detail.empty()) return show.detail;
  switch (show.status) {
    case HostStatus::Busy:
      return "COM busy";
    case HostStatus::Unavailable:
      return "COM unavailable";
    case HostStatus::Failed:
      return "COM failed";
    case HostStatus::Ok:
      break;
  }
  return "COM query failed";
}

} // namespace

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
  const bool was_faulted = machine_.state() == State::Faulted;
  // Unlatch a rate-limited Faulted return retry before ObserveEdge() can see
  // the still-renamed title.
  AllowReturnRetry();
  ObserveShow();
  ObserveCover();
  ObserveEdge();
  if (pending_stop_.has_value()) {
    std::string reason = std::move(*pending_stop_);
    pending_stop_.reset();
    Handle(MakeStopRequested(ports_.clock.now(), std::move(reason)));
  }
  Dispatch(machine_.Tick());
  // Nothing in the machine emits CleanupDone: the runtime owes it the one event
  // that ends a cleanup, and exactly one, so a repeated stop can neither re-run
  // the batch nor re-report it.
  if (!cleanup_done_ && machine_.state() == State::Stopping) {
    cleanup_done_ = true;
    Handle(MakeCleanupDone(ports_.clock.now()));
  }
  SyncSnapshot();
  done_ =
      machine_.state() == State::Exited || machine_.state() == State::SafeMode;
  if (done_) return false;
  // Never idle past the machine's next deadline: a timeout must not be pushed
  // out by a whole poll interval. A deadline that is already due belongs to
  // Tick(); sleeping to it would busy-loop instead of waiting.
  const ClockPoint now = ports_.clock.now();
  // A slide change is only observable through the COM poll, so the poll
  // interval is the trigger latency: poll fast while a show is being watched
  // and fall back to the idle cadence everywhere else.
  const Duration interval = machine_.state() == State::Monitoring
                                ? host_.active_poll_interval
                                : host_.poll_interval;
  ClockPoint wake = now + interval;
  if (const std::optional<ClockPoint> deadline = machine_.NextDeadline();
      deadline.has_value() && *deadline > now && *deadline < wake) {
    wake = *deadline;
  }
  ports_.clock.sleep_until(wake);
  if (!was_faulted && machine_.state() == State::Faulted) {
    // A fault observed in this Step cannot be retried before the next one. The
    // window opens after this iteration's bounded idle wait, so the earliest
    // retry is one full return_retry_interval of loop time after the faulting
    // Step completes.
    last_return_retry_ = ports_.clock.now();
  }
  return true;
}

void WorkerRuntime::RequestStop(std::string reason) {
  pending_stop_ = std::move(reason);
}

void WorkerRuntime::HandleSupervisorLost(std::string detail) {
  Handle(MakeSupervisorLost(ports_.clock.now(), std::move(detail)));
}

void WorkerRuntime::ObserveShow() {
  // Spec 6.6 fault injection: an operator can force the COM failure paths on a
  // real machine without wedging WPS itself.
  if (host_.faults.com_hang || host_.faults.com_462) {
    last_com_failed_ = true;
    const std::string detail =
        host_.faults.com_462 ? "COM busy: 0x462 (injected)"
                             : "COM hang (injected)";
    Handle(MakeFailure(ports_.clock.now(), FailureKind::Transient,
                       FailureSource::Com, detail));
    return;
  }
  const ShowState show = ports_.presentation.QueryShow();
  const ClockPoint now = ports_.clock.now();
  if (show.status == HostStatus::Busy ||
      show.status == HostStatus::Unavailable ||
      show.status == HostStatus::Failed) {
    // A failed COM query is not a show state: keep the last good sample so the
    // failed poll cannot look like a slide change or a show end. Only a real
    // sample may update last_show_/last_slide_.
    last_com_failed_ = true;
    Handle(MakeFailure(now, FailureKind::Transient, FailureSource::Com,
                       ComFailureDetail(show)));
    return;
  }
  if (last_com_failed_) {
    // Spec 3.6: one success after a run of failures closes the error window.
    // Emitted only on the failure-to-success edge, and the machine keeps this
    // path journal-free, so healthy polling stays quiet.
    last_com_failed_ = false;
    Handle(MakeComHealthy(now));
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
  // Spec 6.6: cover-hang models an overlay that never reports its hold, which
  // is what the state machine's cover deadline must survive.
  if (host_.faults.cover_hang) return;
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
    edge_ready_not_before_.reset();
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
  // Spec 6.6: focus-deny models an activation that always fails, and edge-slow
  // models a cold start whose window appears late; both must be survived by the
  // machine's EdgeStarting deadline, so neither blocks here.
  if (host_.faults.focus_deny) return;
  if (edge_ready_not_before_.has_value() && now < *edge_ready_not_before_) {
    return;
  }
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
}

void WorkerRuntime::AppendJournal(std::string entry) {
  // Keep the newest entries: the oldest are the least useful once the cap is
  // reached, and dropping them bounds memory for the life of the run.
  if (journal_.size() >= kMaxJournalEntries) {
    journal_.erase(journal_.begin());
  }
  journal_.push_back(std::move(entry));
}

void WorkerRuntime::AllowReturnRetry() {
  if (machine_.state() != State::Faulted || !return_sent_) return;
  if (ports_.clock.now() - last_return_retry_ < host_.return_retry_interval) {
    return;
  }
  // Unlatch so the next ObserveEdge() may report the still-renamed title once
  // and the machine can retry. The window opened after the faulting Step, so a
  // wedged WPS is retried at most once per return_retry_interval instead of on
  // every poll, and never within the Step that observed the fault.
  return_sent_ = false;
}

void WorkerRuntime::Dispatch(const std::vector<Action>& actions) {
  for (const auto& action : actions) Execute(action);
}

void WorkerRuntime::Execute(const Action& action) {
  // An action may block up to its controller/COM timeout: WPS COM has no
  // non-blocking form, and FocusWps keeps the controller's retry loop. The
  // bounded poll interval governs the loop's idle wait at the end of Step(),
  // not an in-flight action.
  //
  // A failing action re-enters the machine (Execute -> Handle -> Dispatch) in
  // the middle of this batch; the outer batch then continues, so a later
  // [ResumeShow, FocusWps] pair still focuses after the failed resume. That
  // recursion is safe only because the machine's failure handlers do not
  // re-emit the same retrying action immediately (they schedule a retry,
  // escalate, or enter Faulted).
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
      AppendJournal(action.detail);
      break;
    case ActionKind::Diagnostic:
      // Diagnostics are the records an operator or a post-mortem tool reads
      // first, so they carry a stable prefix instead of looking like a journal
      // step.
      AppendJournal("diagnostic: " + action.detail);
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
      edge_ready_not_before_.reset();
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
      if (!host_.faults.focus_deny && edge_pid_ != 0) {
        focus_.TryFocusEdge(edge_.pid());
      }
      break;
    case ActionKind::FocusWps:
      // An empty pid list can never match a WPS window, so calling the
      // controller would only burn its full focus timeout.
      if (!host_.faults.focus_deny && !host_.wps_pids.empty()) {
        focus_.FocusWps(host_.wps_pids, host_.screen);
      }
      break;
    // Kinds owned by later tasks. They are journaled rather than ignored so a
    // missing case stays visible instead of vanishing in a default branch. The
    // cleanup steps get their own labels: journaling the raw event detail twice
    // made the log look like the stop was requested twice.
    case ActionKind::EnterSafeMode:
    case ActionKind::LegacyFallback:
      AppendJournal(action.detail);
      break;
    case ActionKind::StopHelperThreads:
      AppendJournal("cleanup: helper threads stop with the session (" +
                    action.detail + ")");
      break;
    case ActionKind::ReleaseLock:
      AppendJournal("cleanup: release the watcher lock (" + action.detail +
                    ")");
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
  ResolvedTarget launch_target = *target;
  // Staging copies the page (and, in tree mode, its assets) into the run's
  // temp directory so Edge opens a predictable ASCII name instead of the
  // source path. Failure here is reported like a launch failure: the machine's
  // retry policy owns the next attempt.
  if (launch_target.assets_mode != "direct") {
    const StageOutcome staged =
        StageTargetHtml(launch_target, host_.temp_dir / "stage",
                        launch_target.assets_max_bytes);
    if (!staged.staged) {
      ReportEdgeLaunchFailure("staging failed: " + staged.error);
      return;
    }
    launch_target.html_path = Utf8Path(staged.html_path);
    AppendJournal("staged " + launch_target.stage_name + " (" +
                  std::to_string(staged.copied_bytes) + " bytes)");
  }
  const EdgeLaunchSpec spec =
      MakeEdgeLaunchSpec(host_.edge_exe, host_.temp_dir, launch_target,
                         host_.edge);
  if (!edge_.Launch(spec)) {
    // No pid to observe: the machine's retry policy owns the next attempt.
    edge_pid_ = 0;
    ready_sent_ = false;
    return_sent_ = false;
    edge_ready_not_before_.reset();
    ReportEdgeLaunchFailure("edge launch failed");
    return;
  }
  edge_pid_ = edge_.pid();
  ready_sent_ = false;
  return_sent_ = false;
  edge_ready_not_before_.reset();
  if (host_.faults.edge_slow) {
    // Spec 6.6: hold readiness back for a cold-start window; the machine's
    // EdgeStarting deadline still owns the bound, so a real wedge is unaffected.
    edge_ready_not_before_ = ports_.clock.now() + Duration{3000};
  }
}

void WorkerRuntime::ReportEdgeLaunchFailure(std::string detail) {
  AppendJournal(detail);
  Handle(MakeFailure(ports_.clock.now(), FailureKind::Transient,
                     FailureSource::Edge, std::move(detail)));
}

} // namespace htmlppt
