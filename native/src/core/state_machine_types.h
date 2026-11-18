#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/retry_policy.h"
#include "core/types.h"

namespace htmlppt {

enum class ActionKind {
  CoverFadeIn,
  CoverFadeOut,
  CoverHide,
  LaunchEdge,
  KillEdge,
  ExitShow,
  ResumeShow,
  RestoreShow,
  FocusEdge,
  FocusWps,
  EnterSafeMode,
  LegacyFallback,
  StopHelperThreads,
  ReleaseLock,
  Journal,
  Diagnostic,
};

struct Action {
  ActionKind kind = ActionKind::Journal;
  int slide = 0;
  std::string detail;
};

enum class EventKind {
  SlideChanged,
  ShowStarted,
  ShowEnded,
  CoverHold,
  CoverDone,
  EdgeReady,
  EdgeClosed,
  ReturnRequested,
  Timeout,
  StopRequested,
  CleanupDone,
  PowerResume,
  ConfigReloaded,
  Failure,
  SupervisorLost,
  ComHealthy,
};

enum class TimeoutKind {
  CoverHold,
  EdgeReady,
  EdgeLive,
  ReturnResume,
  CoverDone,
  Retry,
};

enum class FailureSource {
  Com,
  Focus,
  Edge,
  Title,
  Other,
};

enum class SupervisorLossPolicy {
  Continue,
  Cleanup,
};

struct Event {
  EventKind kind = EventKind::Timeout;
  ClockPoint at{};
  int old_slide = 0;
  int new_slide = 0;
  TimeoutKind timeout = TimeoutKind::Retry;
  FailureKind failure = FailureKind::Transient;
  FailureSource failure_source = FailureSource::Other;
  std::string detail;
  // PowerResume only: the suspend gap measured by the host. Pending deadlines
  // are shifted by it so a resume does not look like a pile of expired timers.
  Duration jump{};
};

struct TargetPlan {
  int slide = 0;
  std::string label;
  std::string edge_profile;
  TriggerMode trigger = TriggerMode::LeaveForward;
  OnFailurePolicy on_failure = OnFailurePolicy::SelfHeal;
  std::optional<int> max_consecutive_trigger_failures;
  std::optional<Duration> cover_max_hold;
  // Per-target overrides carried from the config/host handoff. Unset means the
  // machine's or the host's global value applies; the app layer fills the screen
  // fields because display metrics are a platform fact.
  std::optional<Duration> edge_max_live = std::nullopt;
  std::optional<Duration> focus_timeout = std::nullopt;
  std::optional<std::string> focus_strategy = std::nullopt;
  std::optional<int> screen_width = std::nullopt;
  std::optional<int> screen_height = std::nullopt;
  bool focus_require = true;
  bool enabled = true;
};

struct RunPlan {
  std::string run_id;
  int slide_count = 0;
  std::vector<TargetPlan> targets;  // enabled targets, sorted by slide
};

struct StateMachineConfig {
  Duration startup_ignore{1000};
  Duration return_ignore{2000};
  // Host-facing overlay/focus timings; the state machine does not read
  // cover_fade, cover_hold, or focus_timeout directly.
  Duration cover_fade{300};
  Duration cover_hold{1000};
  Duration cover_max_hold{10000};
  Duration edge_ready_timeout{20000};
  Duration edge_max_live{600000};
  Duration focus_timeout{6000};
  Duration com_error_window{2000};
  int max_consecutive_trigger_failures = 3;
  OnFailurePolicy default_on_failure = OnFailurePolicy::SelfHeal;
  SupervisorLossPolicy supervisor_loss = SupervisorLossPolicy::Continue;
  RetryConfig retry;
};

struct StateSnapshot {
  State state = State::Boot;
  int prev_slide = -1;
  std::optional<int> current_target_slide;
  int consecutive_trigger_failures = 0;
  std::uint64_t trigger_generation = 0;
  bool return_prepared = false;
  bool degraded_no_supervisor = false;
  bool safe_mode = false;
  bool show_active = false;
  std::optional<ClockPoint> ignore_until;
};

Event MakeSlideChanged(ClockPoint at, int old_slide, int new_slide);
Event MakeShowStarted(ClockPoint at);
Event MakeShowEnded(ClockPoint at);
Event MakeCoverHold(ClockPoint at);
Event MakeCoverDone(ClockPoint at);
Event MakeEdgeReady(ClockPoint at);
Event MakeEdgeClosed(ClockPoint at);
Event MakeReturnRequested(ClockPoint at);
Event MakeTimeout(ClockPoint at, TimeoutKind kind);
Event MakeStopRequested(ClockPoint at, std::string source);
Event MakeCleanupDone(ClockPoint at);
Event MakePowerResume(ClockPoint at, Duration jump = {});
Event MakeFailure(ClockPoint at, FailureKind kind, FailureSource source,
                  std::string detail);
Event MakeSupervisorLost(ClockPoint at, std::string detail);
Event MakeComHealthy(ClockPoint at);

} // namespace htmlppt
