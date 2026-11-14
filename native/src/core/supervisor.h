#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/retry_policy.h"
#include "core/supervisor_protocol.h"

namespace htmlppt {

enum class SupervisorActionKind {
  StartWorker,
  RequestDump,
  TerminateWorker,
  RestartWorker,
  EnterSafeMode,
  RunLegacyFallback,
  StopComplete,
};

struct SupervisorAction {
  SupervisorActionKind kind = SupervisorActionKind::StartWorker;
  std::string detail;
};

struct SupervisorConfig {
  Duration heartbeat_timeout{2000};
  int max_restarts = 5;
  Duration restart_window{600000};
  std::vector<Duration> backoff{
      Duration{200}, Duration{500}, Duration{1000}, Duration{2000},
      Duration{5000}};
  double jitter_ratio = 0.2;
  std::uint32_t seed = 0x9e3779b9u;
  bool legacy_fallback = false;
};

enum class WorkerEventKind {
  Started,
  Heartbeat,
  Exited,
  Hung,
  StopRequested,
  CleanupDone,
};

struct WorkerEvent {
  WorkerEventKind kind = WorkerEventKind::Heartbeat;
  ClockPoint at{};
  std::uint32_t sequence = 0;
  int exit_code = 0;
  std::string detail;
};

class SupervisorCore {
 public:
  SupervisorCore(SupervisorConfig config, Clock& clock);

  std::vector<SupervisorAction> Start();
  std::vector<SupervisorAction> Handle(const WorkerEvent& event);
  std::vector<SupervisorAction> Tick();
  std::optional<ClockPoint> NextDeadline() const { return restart_deadline_; }
  bool safe_mode() const { return safe_mode_; }
  int restart_count() const { return restart_count_; }
  bool worker_running() const { return worker_running_; }
  bool stopping() const { return stopping_; }

 private:
  std::optional<Duration> BackoffFor(int attempt);
  std::uint32_t NextRandom();
  std::vector<SupervisorAction> EnterSafeMode(std::string detail);
  std::vector<SupervisorAction> ScheduleRestart(ClockPoint at,
                                                std::string detail);

  SupervisorConfig config_;
  Clock& clock_;
  SequenceTracker sequence_;
  RestartBudget budget_;
  bool worker_running_ = false;
  bool stopping_ = false;
  bool safe_mode_ = false;
  int restart_count_ = 0;
  std::optional<ClockPoint> restart_deadline_;
  std::uint32_t random_state_ = 0x9e3779b9u;
};

} // namespace htmlppt
