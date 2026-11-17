#include "app/worker_session.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include "core/blackbox.h"
#include "core/supervisor_protocol.h"
#include "core/types.h"
#include "platform/win32/named_pipe.h"
#include "platform/win32/shared_blackbox.h"

namespace htmlppt {
namespace {

constexpr char kStopReason[] = "supervisor stop";
constexpr char kLossDetail[] = "supervisor pipe closed";

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A non-positive interval would turn the heartbeat into a busy loop.
Duration HeartbeatInterval(const WorkerSessionConfig& config) {
  return config.heartbeat_interval.count() > 0 ? config.heartbeat_interval
                                               : Duration{1};
}

// True when this heartbeat is due a blackbox record: the first one, so --status
// shows the worker from the start, and then every `every` heartbeats.
bool RecordDue(int heartbeat_count, int every) {
  if (heartbeat_count <= 0) return false;
  if (heartbeat_count == 1) return true;
  return every > 0 && heartbeat_count % every == 0;
}

Frame MakeHeartbeat(std::uint32_t sequence, State state) {
  Frame frame;
  frame.header.type = FrameType::Heartbeat;
  frame.header.sequence = sequence;
  frame.header.timestamp_ms = static_cast<std::uint64_t>(NowMs());
  frame.payload.push_back(static_cast<std::uint8_t>(state));
  return frame;
}

Frame MakeCleanup(std::uint32_t sequence) {
  Frame frame;
  frame.header.type = FrameType::Cleanup;
  frame.header.sequence = sequence;
  frame.header.timestamp_ms = static_cast<std::uint64_t>(NowMs());
  return frame;
}

void AppendHeartbeatRecord(SharedBlackbox& blackbox, std::uint32_t sequence,
                           State state) {
  BlackboxRecord record;
  record.timestamp_ms = NowMs();
  record.event = BlackboxEvent::Heartbeat;
  record.pid = GetCurrentProcessId();
  record.state = static_cast<std::int32_t>(state);
  record.value = sequence;
  std::snprintf(record.detail, sizeof(record.detail), "worker heartbeat");
  blackbox.Append(record);
}

} // namespace

bool ValidateWorkerPlanRunId(const WorkerPlan& plan, std::string_view run_id,
                             std::string* error) {
  if (plan.machine.run_id == run_id) {
    if (error != nullptr) error->clear();
    return true;
  }
  if (error != nullptr) {
    *error = "worker plan run id \"" + plan.machine.run_id +
             "\" does not match --run-id \"" + std::string(run_id) + "\"";
  }
  return false;
}

std::uint32_t RunWithStartupHeartbeats(FrameChannel& channel,
                                       SharedBlackbox& blackbox,
                                       WorkerSessionConfig config,
                                       const std::function<void()>& startup,
                                       bool* pipe_alive) {
  const Duration interval = HeartbeatInterval(config);
  std::atomic<bool> finished{false};
  // The flag is an output: the caller learns whether the channel survived.
  std::atomic<bool> alive{true};
  std::atomic<std::uint32_t> last_sequence{0};

  std::thread heartbeat([&] {
    std::uint32_t sequence = 0;
    int heartbeat_count = 0;
    for (;;) {
      if (alive.load(std::memory_order_relaxed)) {
        ++sequence;
        last_sequence.store(sequence, std::memory_order_relaxed);
        if (!channel.Send(MakeHeartbeat(sequence, State::Boot))) {
          alive.store(false, std::memory_order_relaxed);
        } else {
          ++heartbeat_count;
          if (RecordDue(heartbeat_count, config.blackbox_every)) {
            AppendHeartbeatRecord(blackbox, sequence, State::Boot);
          }
        }
      }
      if (finished.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(interval);
    }
  });

  // The caller keeps the COM apartment: WPS automation objects are created on
  // this thread, and the heartbeat thread only touches the pipe and the box.
  // The join must happen on every path, including the (unlikely) throwing one,
  // or the joinable thread would terminate the process.
  try {
    startup();
  } catch (...) {
    finished.store(true, std::memory_order_release);
    heartbeat.join();
    throw;
  }
  finished.store(true, std::memory_order_release);
  heartbeat.join();
  if (pipe_alive != nullptr) {
    *pipe_alive = alive.load(std::memory_order_relaxed);
  }
  return last_sequence.load(std::memory_order_relaxed);
}

int RunWorkerSession(WorkerDriver& driver, FrameChannel& channel,
                     SharedBlackbox& blackbox, WorkerSessionConfig config,
                     std::string* error) {
  if (error != nullptr) error->clear();
  const Duration interval = HeartbeatInterval(config);
  std::uint32_t sequence = config.initial_sequence;
  int heartbeat_count = 0;
  bool pipe_alive = !channel.broken();
  bool loss_reported = false;

  const auto report_loss = [&](const char* detail) {
    if (loss_reported) return;
    loss_reported = true;
    driver.HandleSupervisorLost(detail);
    if (error != nullptr) *error = std::string("supervisor lost: ") + detail;
  };

  driver.Start();
  if (!pipe_alive) report_loss(kLossDetail);
  auto last_heartbeat = std::chrono::steady_clock::now();
  while (!driver.done()) {
    if (pipe_alive) {
      const auto command = channel.Receive(Duration{0});
      if (command.has_value()) {
        // Restart frames are the supervisor's job: it terminates this worker
        // and launches a new one, so the session only acts on Stop.
        if (command->header.type == FrameType::Stop) {
          driver.RequestStop(kStopReason);
        }
      } else if (channel.broken()) {
        pipe_alive = false;
        report_loss(kLossDetail);
      }
    }
    if (driver.done()) break;
    if (!driver.Step()) break;

    const auto now = std::chrono::steady_clock::now();
    if (!pipe_alive || now - last_heartbeat < interval) continue;
    last_heartbeat = now;
    ++sequence;
    if (!channel.Send(MakeHeartbeat(sequence, driver.snapshot().state))) {
      pipe_alive = false;
      report_loss(kLossDetail);
      continue;
    }
    ++heartbeat_count;
    if (RecordDue(heartbeat_count, config.blackbox_every)) {
      AppendHeartbeatRecord(blackbox, sequence, driver.snapshot().state);
    }
  }

  // A worker that finished on its own still reports the clean exit so the
  // supervisor can tell a stop from a crash; a dead pipe has no one to tell.
  if (pipe_alive && !channel.Send(MakeCleanup(++sequence))) {
    if (error != nullptr) *error = "cleanup frame was not delivered";
  }
  return 0;
}

} // namespace htmlppt

#endif // _WIN32
