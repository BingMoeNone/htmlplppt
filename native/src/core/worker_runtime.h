#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/config.h"
#include "core/cover_controller.h"
#include "core/cover_timeline.h"
#include "core/edge_controller.h"
#include "core/faults.h"
#include "core/focus_controller.h"
#include "core/latency_stats.h"
#include "core/presentation_host.h"
#include "core/state_machine.h"
#include "core/state_machine_types.h"
#include "core/window_system.h"
#include "core/worker_driver.h"
#include "core/worker_plan.h"

namespace htmlppt {

// Caller-owned ports. They must outlive the runtime.
struct WorkerPorts {
  PresentationHost& presentation;
  CoverOverlay& overlay;
  EdgeLauncher& launcher;
  WindowSystem& windows;
  Clock& clock;
};

struct WorkerHostConfig {
  std::filesystem::path temp_dir;
  std::filesystem::path edge_exe;
  EdgeConfig edge;
  CoverTiming cover;
  FocusTiming focus;
  ScreenMetrics screen;
  std::vector<std::uint32_t> wps_pids;
  // Spec 6.6 fault switches. Off in production; see core/faults.h.
  Faults faults;
  // Minimum spacing between two return attempts after a return failure. The
  // page keeps its renamed title, so without this the Faulted retry would fire
  // on every poll and hammer a wedged WPS with COM calls.
  Duration return_retry_interval{2000};
  // Idle cadence, and the faster cadence used while Monitoring a running show
  // (slide changes are only visible through the COM poll, so the poll interval
  // is the trigger latency).
  Duration active_poll_interval{20};
  Duration poll_interval{100};
};

// Samples the ports into state-machine events, executes the resulting
// actions, and keeps the machine ticking. The state machine remains the only
// state owner; ports only report facts. Implements WorkerDriver so the app's
// session (pipe, heartbeats, stop, supervisor loss) is testable without it.
class WorkerRuntime : public WorkerDriver {
 public:
  WorkerRuntime(WorkerPlan plan, StateMachineConfig config, WorkerPorts ports,
                WorkerHostConfig host);

  WorkerRuntime(const WorkerRuntime&) = delete;
  WorkerRuntime& operator=(const WorkerRuntime&) = delete;

  void Start() override;
  bool Step() override;
  bool done() const override { return done_; }
  void RequestStop(std::string reason) override;
  void HandleSupervisorLost(std::string detail) override;
  const StateSnapshot& snapshot() const override { return snapshot_; }
  const std::vector<std::string>& journal() const { return journal_; }
  // --benchmark instrumentation: per-step and per-COM-query latencies.
  const LatencyStats& step_stats() const { return step_stats_; }
  const LatencyStats& com_stats() const { return com_stats_; }

 private:
  void ObserveShow();
  void ObserveCover();
  void ObserveEdge();
  void LaunchEdge(int slide);
  void ReportEdgeLaunchFailure(std::string detail);
  void SyncSnapshot();
  void AllowReturnRetry();
  void AppendJournal(std::string entry);
  void Handle(const Event& event);
  void Dispatch(const std::vector<Action>& actions);
  void Execute(const Action& action);
  // Applies the in-flight target's focus/screen overrides on top of the host
  // baseline. Called when a trigger starts; a target without an override keeps
  // the baseline, so the previous target's values never leak forward.
  void ApplyTargetSettings(int slide);
  const TargetPlan* FindTarget(int slide) const;
  // focus.require=false makes FocusWps inert; a slide without a target keeps the
  // default requirement so the show-start focus still runs.
  bool FocusRequired(int slide) const;

  WorkerPlan plan_;
  WorkerHostConfig host_;
  // The global focus/screen values from the host config. Every trigger derives
  // the effective settings from these instead of restoring the shared host
  // fields after the target finishes.
  FocusTiming base_focus_;
  ScreenMetrics base_screen_;
  WorkerPorts ports_;
  CoverController cover_;
  EdgeController edge_;
  FocusController focus_;
  StateMachine machine_;
  std::vector<std::string> journal_;
  StateSnapshot snapshot_;
  ShowState last_show_;
  int last_slide_ = 0;
  // True while the most recent COM show poll failed. The first Ok poll after
  // one or more failures reports ComHealthy so the machine resets its
  // contiguous-error window instead of escalating isolated errors (spec 3.6).
  bool last_com_failed_ = false;
  // The previous cover tick's phase edges. Hold/done are emitted on a rising
  // edge only, so the poll loop cannot flood the machine with duplicates.
  bool last_cover_hold_ = false;
  bool last_cover_done_ = false;
  // The process this runtime launched, or 0 when no launch is outstanding.
  // ObserveEdge stays idle at 0 so a fresh runtime cannot report a closure, and
  // KillEdge clears it because the runtime itself ended that process.
  std::uint32_t edge_pid_ = 0;
  // Readiness and return are reported once per launch: EdgeReady once focused,
  // ReturnRequested once even though the page keeps its renamed title. After a
  // return failure the signal is unlatched again at most once per
  // return_retry_interval.
  bool ready_sent_ = false;
  bool return_sent_ = false;
  // Set only by the edge-slow fault: readiness is not reported before this
  // point, modelling a browser whose window appears late.
  std::optional<ClockPoint> edge_ready_not_before_;
  // When the current Faulted retry window opened: after the bounded idle wait
  // of the Step that observed the fault. AllowReturnRetry() measures
  // return_retry_interval from this point.
  ClockPoint last_return_retry_{};
  std::optional<std::string> pending_stop_;
  // Set once the runtime has fed the machine the CleanupDone it never emits
  // itself, so a repeated stop cannot run or report the cleanup twice.
  bool cleanup_done_ = false;
  bool done_ = false;
  // --benchmark samples, bounded by the histogram's ring.
  LatencyStats step_stats_;
  LatencyStats com_stats_;
  // Wall time of the previous Step, used to measure a resume gap.
  ClockPoint last_step_time_{};
  bool injected_sleep_resume_ = false;
  bool injected_multi_monitor_ = false;
};

} // namespace htmlppt
