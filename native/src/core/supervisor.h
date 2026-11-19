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
  // Sequence liveness: the heartbeat helper sends every 200 ms, so a dead
  // helper is a real defect and 2 s is 10x headroom.
  Duration heartbeat_timeout{2000};
  // Loop-progress liveness: a bounded blocking COM action (ResumeShow can run
  // StartShow + FocusShow + JumpTo with retries) can legitimately exceed 2 s,
  // so progress gets a threshold deliberately above the longest permitted
  // blocking action. A true hang is therefore recovered in ~12 s, not the
  // spec's 5 s target; that trade-off avoids killing a healthy slow worker.
  Duration progress_timeout{12000};
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
  // Loop progress decoded from the heartbeat payload. nullopt means the frame
  // carried no progress, so the supervisor judges that heartbeat by sequence
  // alone (as a pre-progress worker would be).
  std::optional<std::uint64_t> progress;
  int exit_code = 0;
  std::string detail;
};

class SupervisorCore {
 public:
  SupervisorCore(SupervisorConfig config, Clock& clock);

  std::vector<SupervisorAction> Start();
  std::vector<SupervisorAction> Handle(const WorkerEvent& event);
  std::vector<SupervisorAction> Tick();
  // Re-arms the liveness trackers after the worker process was replaced without
  // a restart deadline (config hot reload). The replacement's heartbeat
  // sequence starts over, so the previous sequence would mask every new
  // heartbeat and a healthy worker would look hung once the timeout passed.
  // Ignored while the core is stopping or in safe mode, where no new worker is
  // expected.
  void NoteWorkerRestarted(ClockPoint at);
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
  // The dump/terminate/restart batch shared by an explicit Hung event and the
  // Tick() stall detectors.
  std::vector<SupervisorAction> HangActions(ClockPoint at, std::string detail);

  SupervisorConfig config_;
  Clock& clock_;
  SequenceTracker sequence_;
  ProgressTracker progress_;
  RestartBudget budget_;
  bool worker_running_ = false;
  bool stopping_ = false;
  bool safe_mode_ = false;
  int restart_count_ = 0;
  std::optional<ClockPoint> restart_deadline_;
  std::uint32_t random_state_ = 0x9e3779b9u;
};

} // namespace htmlppt
