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
#include "core/focus_controller.h"
#include "core/presentation_host.h"
#include "core/state_machine.h"
#include "core/state_machine_types.h"
#include "core/window_system.h"
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
  Duration poll_interval{100};
};

// Samples the ports into state-machine events, executes the resulting
// actions, and keeps the machine ticking. The state machine remains the only
// state owner; ports only report facts.
class WorkerRuntime {
 public:
  WorkerRuntime(WorkerPlan plan, StateMachineConfig config, WorkerPorts ports,
                WorkerHostConfig host);

  WorkerRuntime(const WorkerRuntime&) = delete;
  WorkerRuntime& operator=(const WorkerRuntime&) = delete;

  void Start();
  bool Step();
  bool done() const { return done_; }
  void RequestStop(std::string reason);
  const StateSnapshot& snapshot() const { return snapshot_; }
  const std::vector<std::string>& journal() const { return journal_; }

 private:
  void ObserveShow();
  void ObserveCover();
  void Handle(const Event& event);
  void Dispatch(const std::vector<Action>& actions);
  void Execute(const Action& action);

  WorkerPlan plan_;
  WorkerHostConfig host_;
  WorkerPorts ports_;
  CoverController cover_;
  EdgeController edge_;
  FocusController focus_;
  StateMachine machine_;
  std::vector<std::string> journal_;
  StateSnapshot snapshot_;
  ShowState last_show_;
  int last_slide_ = 0;
  // The previous cover tick's phase edges. Hold/done are emitted on a rising
  // edge only, so the poll loop cannot flood the machine with duplicates.
  bool last_cover_hold_ = false;
  bool last_cover_done_ = false;
  std::optional<std::string> pending_stop_;
  bool done_ = false;
};

} // namespace htmlppt
