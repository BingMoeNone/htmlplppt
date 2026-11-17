#include "core/state_machine.h"

#include <utility>

namespace htmlppt {

StateMachine::StateMachine(RunPlan plan, StateMachineConfig config,
                           Clock& clock)
    : plan_(std::move(plan)),
      config_(std::move(config)),
      clock_(clock),
      retry_(config_.retry),
      com_window_(config_.com_error_window) {}

std::vector<Action> StateMachine::Start() {
  if (state_ != State::Boot) return {};
  Enter(State::Warming);
  ignore_until_ = clock_.now() + config_.startup_ignore;
  return Journal("state=Warming");
}

std::vector<Action> StateMachine::Handle(const Event& event) {
  if (state_ == State::Exited) return {};
  switch (event.kind) {
    case EventKind::SlideChanged:
      return OnSlideChanged(event);
    case EventKind::ShowStarted:
      return OnShowStarted(event);
    case EventKind::ShowEnded:
      return OnShowEnded(event);
    case EventKind::CoverHold:
      return OnCoverHold(event);
    case EventKind::CoverDone:
      return OnCoverDone(event);
    case EventKind::EdgeReady:
      return OnEdgeReady(event);
    case EventKind::EdgeClosed:
      return OnEdgeClosed(event);
    case EventKind::ReturnRequested:
      return OnReturnRequested(event);
    case EventKind::Timeout:
      return OnTimeout(event);
    case EventKind::StopRequested:
      return OnStopRequested(event);
    case EventKind::CleanupDone:
      return OnCleanupDone(event);
    case EventKind::PowerResume:
      return OnPowerResume(event);
    case EventKind::ConfigReloaded:
      return Journal("config reload ignored");
    case EventKind::Failure:
      return OnFailure(event);
    case EventKind::SupervisorLost:
      return OnSupervisorLost(event);
    case EventKind::ComHealthy:
      // Spec 3.6: a successful COM read resets the contiguous-error window. The
      // healthy path stays silent (no journal) so a normal poll adds no noise.
      com_window_.reset();
      return {};
  }
  return {};
}

std::vector<Action> StateMachine::Tick() {
  const auto now = clock_.now();
  const auto earliest = NextDeadline();
  if (!earliest.has_value() || now < *earliest) return {};
  const TimeoutKind order[] = {
      TimeoutKind::CoverHold, TimeoutKind::EdgeReady, TimeoutKind::EdgeLive,
      TimeoutKind::ReturnResume, TimeoutKind::CoverDone, TimeoutKind::Retry};
  for (const auto kind : order) {
    const auto deadline = Deadline(kind);
    if (deadline.has_value() && *deadline <= now) {
      return Handle(MakeTimeout(now, kind));
    }
  }
  return {};
}

std::optional<ClockPoint> StateMachine::NextDeadline() const {
  const std::optional<ClockPoint>* deadlines[] = {
      &cover_hold_deadline_, &edge_ready_deadline_, &edge_live_deadline_,
      &return_resume_deadline_, &cover_done_deadline_, &retry_deadline_};
  std::optional<ClockPoint> earliest;
  for (const auto* deadline : deadlines) {
    if (deadline->has_value() &&
        (!earliest.has_value() || **deadline < *earliest)) {
      earliest = **deadline;
    }
  }
  return earliest;
}

StateSnapshot StateMachine::Snapshot() const {
  StateSnapshot snapshot;
  snapshot.state = state_;
  snapshot.prev_slide = prev_slide_;
  snapshot.current_target_slide = current_target_slide_;
  snapshot.consecutive_trigger_failures = consecutive_trigger_failures_;
  snapshot.trigger_generation = trigger_generation_;
  snapshot.return_prepared = return_prepared_;
  snapshot.degraded_no_supervisor = degraded_;
  snapshot.safe_mode = safe_mode_;
  snapshot.show_active = show_active_;
  snapshot.ignore_until = ignore_until_;
  return snapshot;
}

const TargetPlan* StateMachine::FindTarget(int slide) const {
  for (const auto& target : plan_.targets) {
    if (target.slide == slide) return &target;
  }
  return nullptr;
}

int StateMachine::EffectiveFailureThreshold(const TargetPlan& target) const {
  return target.max_consecutive_trigger_failures.value_or(
      config_.max_consecutive_trigger_failures);
}

Duration StateMachine::EffectiveCoverMaxHold(const TargetPlan& target) const {
  return target.cover_max_hold.value_or(config_.cover_max_hold);
}

Duration StateMachine::ReturnCoverMaxHold() const {
  if (!current_target_slide_.has_value()) return config_.cover_max_hold;
  const TargetPlan* target = FindTarget(*current_target_slide_);
  return target != nullptr ? EffectiveCoverMaxHold(*target)
                           : config_.cover_max_hold;
}

void StateMachine::Enter(State state) {
  state_ = state;
  if (state == State::SafeMode) safe_mode_ = true;
}

void StateMachine::ResetRetries() {
  retry_.reset();
  retry_attempt_ = 0;
  pending_retry_ = RetryOperation::None;
  retry_deadline_.reset();
}

void StateMachine::ClearDeadlines() {
  cover_hold_deadline_.reset();
  edge_ready_deadline_.reset();
  edge_live_deadline_.reset();
  return_resume_deadline_.reset();
  cover_done_deadline_.reset();
  retry_deadline_.reset();
}

void StateMachine::SetDeadline(TimeoutKind kind, ClockPoint at) {
  switch (kind) {
    case TimeoutKind::CoverHold:
      cover_hold_deadline_ = at;
      break;
    case TimeoutKind::EdgeReady:
      edge_ready_deadline_ = at;
      break;
    case TimeoutKind::EdgeLive:
      edge_live_deadline_ = at;
      break;
    case TimeoutKind::ReturnResume:
      return_resume_deadline_ = at;
      break;
    case TimeoutKind::CoverDone:
      cover_done_deadline_ = at;
      break;
    case TimeoutKind::Retry:
      retry_deadline_ = at;
      break;
  }
}

void StateMachine::ClearDeadline(TimeoutKind kind) {
  switch (kind) {
    case TimeoutKind::CoverHold:
      cover_hold_deadline_.reset();
      break;
    case TimeoutKind::EdgeReady:
      edge_ready_deadline_.reset();
      break;
    case TimeoutKind::EdgeLive:
      edge_live_deadline_.reset();
      break;
    case TimeoutKind::ReturnResume:
      return_resume_deadline_.reset();
      break;
    case TimeoutKind::CoverDone:
      cover_done_deadline_.reset();
      break;
    case TimeoutKind::Retry:
      retry_deadline_.reset();
      break;
  }
}

std::optional<ClockPoint> StateMachine::Deadline(TimeoutKind kind) const {
  switch (kind) {
    case TimeoutKind::CoverHold:
      return cover_hold_deadline_;
    case TimeoutKind::EdgeReady:
      return edge_ready_deadline_;
    case TimeoutKind::EdgeLive:
      return edge_live_deadline_;
    case TimeoutKind::ReturnResume:
      return return_resume_deadline_;
    case TimeoutKind::CoverDone:
      return cover_done_deadline_;
    case TimeoutKind::Retry:
      return retry_deadline_;
  }
  return std::nullopt;
}

std::vector<Action> StateMachine::OnSlideChanged(const Event& event) {
  if (state_ != State::Monitoring) return {};
  com_window_.reset();
  ResetRetries();
  const int previous = prev_slide_ >= 0 ? prev_slide_ : event.old_slide;
  prev_slide_ = event.new_slide;
  if (ignore_until_.has_value() && event.at < *ignore_until_) return {};
  for (const auto& target : plan_.targets) {
    if (!target.enabled || done_slides_.count(target.slide) != 0) continue;
    const bool matches =
        target.trigger == TriggerMode::Arrive
            ? target.slide == event.new_slide && event.new_slide != previous
            : target.slide == previous && event.new_slide > previous;
    if (matches) return BeginTrigger(target, event.at);
  }
  return {};
}

std::vector<Action> StateMachine::OnShowStarted(const Event& event) {
  if (state_ != State::Warming && state_ != State::Monitoring) return {};
  const bool restarted = state_ == State::Monitoring;
  done_slides_.clear();
  prev_slide_ = -1;
  current_target_slide_.reset();
  return_prepared_ = false;
  consecutive_trigger_failures_ = 0;
  ignore_until_ = event.at + config_.startup_ignore;
  ResetRetries();
  ClearDeadlines();
  com_window_.reset();
  show_active_ = true;
  Enter(State::Monitoring);
  auto actions = Journal(restarted ? "show restarted" : "show started");
  actions.push_back({ActionKind::FocusWps, 0, "focus WPS"});
  return actions;
}

std::vector<Action> StateMachine::OnShowEnded(const Event& event) {
  if (state_ == State::ShowExit) {
    ClearDeadline(TimeoutKind::EdgeReady);
    ResetRetries();
    com_window_.reset();
    Enter(State::Live);
    show_active_ = false;
    consecutive_trigger_failures_ = 0;
    SetDeadline(TimeoutKind::EdgeLive, event.at + config_.edge_max_live);
    auto actions = Journal("live", current_target_slide_.value_or(0));
    actions.push_back({ActionKind::CoverFadeOut,
                       current_target_slide_.value_or(0), "reveal Edge"});
    return actions;
  }
  if (state_ == State::Monitoring) {
    show_active_ = false;
    done_slides_.clear();
    prev_slide_ = -1;
    current_target_slide_.reset();
    return Journal("show ended");
  }
  if (state_ == State::Returning || state_ == State::ReturnCoverOut) {
    return HandleReturnFailure("show ended during return");
  }
  if (state_ == State::Live) {
    return Journal("show ended while live");
  }
  return {};
}

std::vector<Action> StateMachine::OnCoverHold(const Event& event) {
  if (state_ == State::CoverIn) {
    ClearDeadline(TimeoutKind::CoverHold);
    ResetRetries();
    com_window_.reset();
    Enter(State::EdgeStarting);
    SetDeadline(TimeoutKind::EdgeReady, event.at + config_.edge_ready_timeout);
    const int slide = current_target_slide_.value_or(0);
    const TargetPlan* target = FindTarget(slide);
    const std::string profile =
        target ? target->edge_profile : std::string{};
    auto actions = Journal("cover hold", slide);
    actions.push_back({ActionKind::LaunchEdge, slide, profile});
    return actions;
  }
  if (state_ == State::Returning) {
    ClearDeadline(TimeoutKind::ReturnResume);
    ResetRetries();
    com_window_.reset();
    const int slide = current_target_slide_.value_or(0);
    Enter(State::ReturnCoverOut);
    show_active_ = true;
    SetDeadline(TimeoutKind::CoverDone, event.at + ReturnCoverMaxHold());
    auto actions = Journal("resumed at slide " + std::to_string(slide), slide);
    // Close Edge while the cover is still opaque. A page that only renamed its
    // title (the documented TEF-LIVE-RETURN signal) would otherwise stay
    // full-screen on top of the resumed show once the cover fades out. This is
    // deliberately after the return request, so a failed resume can still be
    // retried by watching the title.
    actions.push_back({ActionKind::KillEdge, slide, "return: close Edge"});
    actions.push_back({ActionKind::ResumeShow, slide, "resume WPS"});
    actions.push_back({ActionKind::FocusWps, slide, "focus WPS"});
    actions.push_back({ActionKind::CoverFadeOut, slide, "fade out cover"});
    return actions;
  }
  return {};
}

std::vector<Action> StateMachine::OnCoverDone(const Event& event) {
  if (state_ != State::ReturnCoverOut) return {};
  ClearDeadline(TimeoutKind::CoverDone);
  ResetRetries();
  Enter(State::Monitoring);
  ignore_until_ = event.at + config_.return_ignore;
  prev_slide_ = -1;
  current_target_slide_.reset();
  return_prepared_ = false;
  return Journal("return cover done");
}

std::vector<Action> StateMachine::OnEdgeReady(const Event& event) {
  (void)event;
  if (state_ != State::EdgeStarting) return {};
  ClearDeadline(TimeoutKind::EdgeReady);
  ResetRetries();
  com_window_.reset();
  const int slide = current_target_slide_.value_or(0);
  Enter(State::ShowExit);
  auto actions = Journal("edge ready", slide);
  actions.push_back({ActionKind::ExitShow, slide, "exit WPS show"});
  return actions;
}

std::vector<Action> StateMachine::OnEdgeClosed(const Event& event) {
  if (state_ == State::Live) {
    return BeginReturn(event.at, false,
                       "edge closed after return; cover fade out");
  }
  if (state_ == State::EdgeStarting || state_ == State::ShowExit) {
    return HandleSwitchFailure(event.at, "edge closed before ready");
  }
  if (state_ == State::Faulted) {
    return Journal("edge closed after return failure",
                   current_target_slide_.value_or(0));
  }
  return {};
}

std::vector<Action> StateMachine::OnReturnRequested(const Event& event) {
  if (state_ == State::Live) {
    return BeginReturn(event.at, true, "return requested by page");
  }
  if (state_ == State::Faulted) {
    return BeginReturn(event.at, true, "return requested by page (retry)");
  }
  return {};
}

std::vector<Action> StateMachine::OnTimeout(const Event& event) {
  switch (event.timeout) {
    case TimeoutKind::CoverHold:
      if (state_ == State::CoverIn) {
        ClearDeadline(TimeoutKind::CoverHold);
        return HandleSwitchFailure(event.at, "cover hold timeout");
      }
      break;
    case TimeoutKind::EdgeReady:
      if (state_ == State::EdgeStarting) {
        ClearDeadline(TimeoutKind::EdgeReady);
        return HandleSwitchFailure(event.at, "edge ready timeout");
      }
      break;
    case TimeoutKind::EdgeLive:
      if (state_ == State::Live) {
        ClearDeadline(TimeoutKind::EdgeLive);
        auto actions = Journal("edge live timeout",
                               current_target_slide_.value_or(0));
        actions.push_back({ActionKind::KillEdge,
                           current_target_slide_.value_or(0),
                           "edge live timeout"});
        auto returning = BeginReturn(event.at, false, "edge live timeout");
        actions.insert(actions.end(), returning.begin(), returning.end());
        return actions;
      }
      break;
    case TimeoutKind::ReturnResume:
      if (state_ == State::Returning) {
        ClearDeadline(TimeoutKind::ReturnResume);
        return HandleReturnFailure("resume show timeout");
      }
      break;
    case TimeoutKind::CoverDone:
      if (state_ == State::ReturnCoverOut) {
        ClearDeadline(TimeoutKind::CoverDone);
        const int slide = current_target_slide_.value_or(0);
        Enter(State::Monitoring);
        ignore_until_ = event.at + config_.return_ignore;
        prev_slide_ = -1;
        current_target_slide_.reset();
        return_prepared_ = false;
        auto actions = Journal("cover done timeout; hiding cover", slide);
        actions.push_back(
            {ActionKind::CoverHide, slide, "cover done timeout"});
        actions.push_back(
            {ActionKind::Diagnostic, slide, "cover fade-out timed out"});
        return actions;
      }
      break;
    case TimeoutKind::Retry:
      return ExecuteRetry(event.at);
  }
  return {};
}

std::vector<Action> StateMachine::OnStopRequested(const Event& event) {
  if (state_ == State::Stopping) return {};
  return BeginCleanup("stop requested: " + event.detail);
}

std::vector<Action> StateMachine::OnCleanupDone(const Event& event) {
  (void)event;
  if (state_ != State::Stopping) return {};
  Enter(State::Exited);
  return Journal("cleanup done");
}

std::vector<Action> StateMachine::OnFailure(const Event& event) {
  if (state_ == State::Stopping || state_ == State::Exited ||
      state_ == State::SafeMode) {
    return {};
  }
  if (state_ == State::Faulted) {
    return Journal("failure while faulted: " + event.detail);
  }
  if (event.failure == FailureKind::Structural) {
    return HandleStructuralFailure(event.detail);
  }
  if (event.failure == FailureKind::Switch) {
    return HandleSwitchFailure(event.at, event.detail);
  }
  if (event.failure == FailureKind::Return) {
    return HandleReturnFailure(event.detail);
  }
  if (event.failure_source == FailureSource::Com) {
    com_window_.record(event.at);
    if (com_window_.expired(event.at)) {
      return HandleStructuralFailure("COM busy: " + event.detail);
    }
  }
  RetryOperation operation = RetryOperation::None;
  switch (state_) {
    case State::CoverIn:
      operation = RetryOperation::CoverFadeIn;
      break;
    case State::EdgeStarting:
      operation = RetryOperation::LaunchEdge;
      break;
    case State::ShowExit:
      operation = RetryOperation::ExitShow;
      break;
    case State::Returning:
      operation = RetryOperation::CoverFadeIn;
      break;
    case State::ReturnCoverOut:
      return Journal("transient failure while returning: " + event.detail);
    case State::Live:
      return Journal("transient failure while live: " + event.detail);
    case State::Monitoring:
      return Journal("transient failure while monitoring: " + event.detail);
    case State::Boot:
    case State::Warming:
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
    case State::DegradedNoSupervisor:
      // Stopping/Faulted/SafeMode/Exited are handled by the early returns above;
      // the remaining states are structural failures.
      return HandleStructuralFailure(event.detail);
  }
  return BeginRetry(operation, event.at, event.detail);
}

std::vector<Action> StateMachine::OnPowerResume(const Event& event) {
  // Spec 11.2: after a resume the steady clock has advanced by the whole suspend
  // gap, so every pending absolute deadline would look expired and fire at once.
  // Shift them by the gap instead; the remaining wait is preserved.
  if (event.jump.count() > 0) {
    ShiftDeadlines(event.jump);
  }
  return Journal("power resume: shifted deadlines by " +
                 std::to_string(event.jump.count()) + "ms");
}

void StateMachine::ShiftDeadlines(Duration jump) {
  for (auto* deadline : {&cover_hold_deadline_, &edge_ready_deadline_,
                         &edge_live_deadline_, &return_resume_deadline_,
                         &cover_done_deadline_, &retry_deadline_}) {
    if (deadline->has_value()) **deadline += jump;
  }
}

std::vector<Action> StateMachine::OnSupervisorLost(const Event& event) {
  if (state_ == State::Stopping) return {};
  if (config_.supervisor_loss == SupervisorLossPolicy::Cleanup) {
    return BeginCleanup("supervisor lost: " + event.detail);
  }
  degraded_ = true;
  return Journal("supervisor lost; continuing: " + event.detail);
}

std::vector<Action> StateMachine::BeginTrigger(const TargetPlan& target,
                                               ClockPoint at) {
  done_slides_.insert(target.slide);
  current_target_slide_ = target.slide;
  ++trigger_generation_;
  ResetRetries();
  ClearDeadlines();
  Enter(State::CoverIn);
  SetDeadline(TimeoutKind::CoverHold,
              at + EffectiveCoverMaxHold(target));
  auto actions =
      Journal("trigger slide " + std::to_string(target.slide) + " -> " +
                  target.label,
              target.slide);
  actions.push_back({ActionKind::CoverFadeIn, target.slide, "cover fade in"});
  return actions;
}

std::vector<Action> StateMachine::BeginReturn(ClockPoint at, bool prepared,
                                              std::string detail) {
  const int slide = current_target_slide_.value_or(0);
  return_prepared_ = prepared;
  ResetRetries();
  ClearDeadlines();
  Enter(State::Returning);
  SetDeadline(TimeoutKind::ReturnResume, at + ReturnCoverMaxHold());
  auto actions = Journal(std::move(detail), slide);
  actions.push_back({ActionKind::CoverFadeIn, slide, "return cover fade"});
  return actions;
}

std::vector<Action> StateMachine::BeginRetry(RetryOperation operation,
                                             ClockPoint at,
                                             std::string detail) {
  const auto delay = retry_.next_delay(retry_attempt_);
  if (!delay.has_value()) {
    const FailureKind escalated = EscalateFailure(state_);
    if (escalated == FailureKind::Switch) {
      return HandleSwitchFailure(at, detail + " (retries exhausted)");
    }
    if (escalated == FailureKind::Return) {
      return HandleReturnFailure(detail + " (retries exhausted)");
    }
    return HandleStructuralFailure(detail + " (retries exhausted)");
  }
  ++retry_attempt_;
  pending_retry_ = operation;
  SetDeadline(TimeoutKind::Retry, at + *delay);
  auto actions = Journal("transient failure; retry in " +
                         std::to_string(delay->count()) + "ms: " + detail);
  actions.push_back(
      {ActionKind::Diagnostic, current_target_slide_.value_or(0), detail});
  return actions;
}

std::vector<Action> StateMachine::ExecuteRetry(ClockPoint at) {
  ClearDeadline(TimeoutKind::Retry);
  const RetryOperation operation = pending_retry_;
  pending_retry_ = RetryOperation::None;
  const int slide = current_target_slide_.value_or(0);
  switch (operation) {
    case RetryOperation::CoverFadeIn:
      return {{ActionKind::CoverFadeIn, slide, "retry cover fade"}};
    case RetryOperation::LaunchEdge: {
      SetDeadline(TimeoutKind::EdgeReady, at + config_.edge_ready_timeout);
      const TargetPlan* target = FindTarget(slide);
      return {{ActionKind::KillEdge, slide, "retry launch Edge"},
              {ActionKind::LaunchEdge, slide,
               target ? target->edge_profile : std::string{}}};
    }
    case RetryOperation::ExitShow:
      return {{ActionKind::ExitShow, slide, "retry exit show"}};
    case RetryOperation::None:
      return {};
  }
  return {};
}

std::vector<Action> StateMachine::HandleSwitchFailure(ClockPoint at,
                                                      std::string detail) {
  const int slide = current_target_slide_.value_or(0);
  const TargetPlan* target = FindTarget(slide);
  const OnFailurePolicy policy =
      target ? target->on_failure : config_.default_on_failure;
  const int threshold = target ? EffectiveFailureThreshold(*target)
                               : config_.max_consecutive_trigger_failures;

  ClearDeadlines();
  com_window_.reset();
  ResetRetries();
  ++consecutive_trigger_failures_;

  std::vector<Action> actions;
  actions.push_back({ActionKind::CoverHide, slide, "switch failure"});
  actions.push_back({ActionKind::KillEdge, slide, "switch failure"});
  actions.push_back(
      {ActionKind::Journal, slide, "switch failure: " + detail});

  if (policy == OnFailurePolicy::Safe ||
      policy == OnFailurePolicy::Legacy ||
      consecutive_trigger_failures_ >= threshold) {
    Enter(State::SafeMode);
    show_active_ = false;
    actions.push_back({ActionKind::EnterSafeMode, slide,
                       "switch failure threshold or policy"});
    if (policy == OnFailurePolicy::Legacy) {
      actions.push_back({ActionKind::LegacyFallback, slide, "switch failure"});
    }
    current_target_slide_.reset();
    return actions;
  }

  if (policy == OnFailurePolicy::SelfHeal && slide > 0) {
    done_slides_.erase(slide);
  }
  prev_slide_ = slide > 0 ? slide : -1;
  ignore_until_ = at + config_.return_ignore;
  show_active_ = true;
  Enter(State::Monitoring);
  actions.push_back(
      {ActionKind::RestoreShow, slide, "restore WPS after switch failure"});
  actions.push_back(
      {ActionKind::FocusWps, slide, "focus WPS after switch failure"});
  actions.push_back(
      {ActionKind::Journal, slide,
       "switch failure recovered; monitoring at slide " +
           std::to_string(slide)});
  current_target_slide_.reset();
  return actions;
}

std::vector<Action> StateMachine::HandleReturnFailure(std::string detail) {
  ClearDeadlines();
  ResetRetries();
  return_prepared_ = true;
  show_active_ = false;
  const int slide = current_target_slide_.value_or(0);
  Enter(State::Faulted);
  std::vector<Action> actions;
  actions.push_back({ActionKind::CoverHide, slide, "return failure"});
  actions.push_back(
      {ActionKind::Journal, slide, "return failure: " + detail});
  actions.push_back(
      {ActionKind::Diagnostic, slide,
       "WPS resume failed; close Edge manually or retry"});
  return actions;
}

std::vector<Action> StateMachine::HandleStructuralFailure(std::string detail) {
  ClearDeadlines();
  ResetRetries();
  const int slide = current_target_slide_.value_or(0);
  const TargetPlan* target = FindTarget(slide);
  const OnFailurePolicy policy =
      target ? target->on_failure : config_.default_on_failure;
  Enter(State::SafeMode);
  show_active_ = false;
  std::vector<Action> actions;
  actions.push_back({ActionKind::CoverHide, slide, "structural failure"});
  actions.push_back({ActionKind::KillEdge, slide, "structural failure"});
  actions.push_back(
      {ActionKind::Journal, slide, "structural failure: " + detail});
  // Spec 11.2: entering SafeMode must leave the slideshow and hand WPS back to
  // the operator instead of leaving a full-screen show behind the hidden cover,
  // and must leave a diagnostic record. The cover is hidden first so the
  // operator never stares at a black screen while these run.
  actions.push_back({ActionKind::ExitShow, slide, "safe mode: exit show"});
  actions.push_back({ActionKind::FocusWps, slide, "safe mode: restore WPS"});
  actions.push_back(
      {ActionKind::EnterSafeMode, slide, "structural failure"});
  actions.push_back({ActionKind::Diagnostic, slide, "safe mode: " + detail});
  if (policy == OnFailurePolicy::Legacy) {
    actions.push_back(
        {ActionKind::LegacyFallback, slide, "structural failure"});
  }
  current_target_slide_.reset();
  return actions;
}

std::vector<Action> StateMachine::BeginCleanup(std::string detail) {
  ClearDeadlines();
  ResetRetries();
  const int slide = current_target_slide_.value_or(0);
  show_active_ = false;
  Enter(State::Stopping);
  std::vector<Action> actions;
  actions.push_back({ActionKind::Journal, slide, "cleanup start"});
  actions.push_back({ActionKind::CoverHide, slide, detail});
  // Leave the slideshow before releasing: --stop must not strand a full-screen
  // show on the operator's desktop.
  actions.push_back({ActionKind::ExitShow, slide, detail});
  actions.push_back({ActionKind::KillEdge, slide, detail});
  actions.push_back({ActionKind::StopHelperThreads, slide, detail});
  actions.push_back({ActionKind::ReleaseLock, slide, detail});
  current_target_slide_.reset();
  return actions;
}

std::vector<Action> StateMachine::Journal(std::string detail,
                                          int slide) const {
  return {{ActionKind::Journal, slide, std::move(detail)}};
}

} // namespace htmlppt
