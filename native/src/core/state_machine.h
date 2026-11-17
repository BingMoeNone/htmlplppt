#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/failure_policy.h"
#include "core/retry_policy.h"
#include "core/state_machine_types.h"

namespace htmlppt {

class StateMachine {
 public:
  StateMachine(RunPlan plan, StateMachineConfig config, Clock& clock);

  std::vector<Action> Start();
  std::vector<Action> Handle(const Event& event);
  std::vector<Action> Tick();
  std::optional<ClockPoint> NextDeadline() const;
  StateSnapshot Snapshot() const;
  State state() const { return state_; }
  bool IsDone(int slide) const { return done_slides_.count(slide) != 0; }

 private:
  enum class RetryOperation {
    None,
    CoverFadeIn,
    LaunchEdge,
    ExitShow,
  };

  const TargetPlan* FindTarget(int slide) const;
  int EffectiveFailureThreshold(const TargetPlan& target) const;
  Duration EffectiveCoverMaxHold(const TargetPlan& target) const;
  // The cover max hold for the in-flight target, used by the return paths so a
  // per-target cover.maxHoldMs also bounds the return wait.
  Duration ReturnCoverMaxHold() const;
  void Enter(State state);
  void ResetRetries();
  void ClearDeadlines();
  void SetDeadline(TimeoutKind kind, ClockPoint at);
  void ClearDeadline(TimeoutKind kind);
  std::optional<ClockPoint> Deadline(TimeoutKind kind) const;

  std::vector<Action> OnSlideChanged(const Event& event);
  std::vector<Action> OnShowStarted(const Event& event);
  std::vector<Action> OnShowEnded(const Event& event);
  std::vector<Action> OnCoverHold(const Event& event);
  std::vector<Action> OnCoverDone(const Event& event);
  std::vector<Action> OnEdgeReady(const Event& event);
  std::vector<Action> OnEdgeClosed(const Event& event);
  std::vector<Action> OnReturnRequested(const Event& event);
  std::vector<Action> OnTimeout(const Event& event);
  std::vector<Action> OnStopRequested(const Event& event);
  std::vector<Action> OnCleanupDone(const Event& event);
  std::vector<Action> OnFailure(const Event& event);
  std::vector<Action> OnSupervisorLost(const Event& event);

  std::vector<Action> BeginTrigger(const TargetPlan& target, ClockPoint at);
  std::vector<Action> BeginReturn(ClockPoint at, bool prepared,
                                  std::string detail);
  std::vector<Action> BeginRetry(RetryOperation operation, ClockPoint at,
                                 std::string detail);
  std::vector<Action> ExecuteRetry(ClockPoint at);
  std::vector<Action> HandleSwitchFailure(ClockPoint at, std::string detail);
  std::vector<Action> HandleReturnFailure(std::string detail);
  std::vector<Action> HandleStructuralFailure(std::string detail);
  std::vector<Action> BeginCleanup(std::string detail);
  std::vector<Action> Journal(std::string detail, int slide = 0) const;

  RunPlan plan_;
  StateMachineConfig config_;
  Clock& clock_;
  RetryPolicy retry_;
  ComErrorWindow com_window_;

  State state_ = State::Boot;
  int prev_slide_ = -1;
  std::set<int> done_slides_;
  std::optional<int> current_target_slide_;
  // Diagnostic/status only. Duplicate-resume prevention is enforced by the
  // Returning/ReturnCoverOut state gates, not by this flag.
  bool return_prepared_ = false;
  bool degraded_ = false;
  bool safe_mode_ = false;
  bool show_active_ = false;
  int consecutive_trigger_failures_ = 0;
  std::uint64_t trigger_generation_ = 0;
  std::optional<ClockPoint> ignore_until_;
  std::optional<ClockPoint> cover_hold_deadline_;
  std::optional<ClockPoint> edge_ready_deadline_;
  std::optional<ClockPoint> edge_live_deadline_;
  std::optional<ClockPoint> return_resume_deadline_;
  std::optional<ClockPoint> cover_done_deadline_;
  std::optional<ClockPoint> retry_deadline_;
  RetryOperation pending_retry_ = RetryOperation::None;
  int retry_attempt_ = 0;
};

} // namespace htmlppt
