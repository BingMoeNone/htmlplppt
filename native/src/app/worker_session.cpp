#include "app/worker_session.h"

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
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
// Spec 11.3: stop-live.vbs removes watcher.lock; the worker treats the removal
// as the same request, so old tooling keeps working.
constexpr char kLegacyStopReason[] = "legacy stop: watcher.lock removed";
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

Frame MakeHeartbeat(std::uint32_t sequence, State state,
                    std::uint64_t progress, bool at_safe_point) {
  Frame frame;
  frame.header.type = FrameType::Heartbeat;
  frame.header.sequence = sequence;
  frame.header.timestamp_ms = static_cast<std::uint64_t>(NowMs());
  HeartbeatPayload payload;
  payload.state = static_cast<std::uint8_t>(state);
  payload.progress = progress;
  payload.flags = at_safe_point ? kHeartbeatFlagSafePoint : 0;
  frame.payload = EncodeHeartbeatPayload(payload);
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

// Sends heartbeats on its own thread for as long as it is alive, so a blocking
// Start/Step (WPS COM activation, ResumeShow, ExitShow) cannot leave the
// supervisor's heartbeat window empty. The caller publishes the last state it
// observed and the driver owns the loop's real safe point, so the sender may
// carry a state one step stale, which the supervisor only logs, while hot reload
// keys on the safe-point flag rather than the state byte. Every send takes
// `send_mutex`, which the caller also holds for the final Cleanup frame.
class HeartbeatSender {
 public:
  HeartbeatSender(FrameChannel& channel, SharedBlackbox& blackbox,
                  WorkerSessionConfig config,
                  const std::atomic<int>& published_state,
                  const std::atomic<std::uint64_t>& published_progress,
                  const std::atomic<bool>& safe_point,
                  std::mutex& send_mutex)
      : channel_(channel),
        blackbox_(blackbox),
        config_(config),
        published_state_(published_state),
        published_progress_(published_progress),
        safe_point_(safe_point),
        send_mutex_(send_mutex),
        interval_(HeartbeatInterval(config)) {
    thread_ = std::thread([this] { Run(); });
  }

  ~HeartbeatSender() { Stop(); }

  HeartbeatSender(const HeartbeatSender&) = delete;
  HeartbeatSender& operator=(const HeartbeatSender&) = delete;

  // Wakes the sender and joins it; safe to call more than once.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(stop_mutex_);
      stopping_ = true;
    }
    stop_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  std::uint32_t last_sequence() const {
    return last_sequence_.load(std::memory_order_relaxed);
  }

  // True when a heartbeat send failed, which means the supervisor is gone.
  bool broken() const { return broken_.load(std::memory_order_relaxed); }

 private:
  void Run() {
    std::uint32_t sequence = config_.initial_sequence;
    int heartbeat_count = 0;
    for (;;) {
      const State state =
          static_cast<State>(published_state_.load(std::memory_order_relaxed));
      const std::uint64_t progress =
          published_progress_.load(std::memory_order_relaxed);
      const bool at_safe_point =
          safe_point_.load(std::memory_order_relaxed);
      ++sequence;
      last_sequence_.store(sequence, std::memory_order_relaxed);
      bool sent = false;
      {
        std::lock_guard<std::mutex> lock(send_mutex_);
        sent = channel_.Send(
            MakeHeartbeat(sequence, state, progress, at_safe_point));
      }
      if (!sent) {
        broken_.store(true, std::memory_order_relaxed);
        return;
      }
      ++heartbeat_count;
      if (RecordDue(heartbeat_count, config_.blackbox_every)) {
        AppendHeartbeatRecord(blackbox_, sequence, state);
      }
      if (config_.on_heartbeat) config_.on_heartbeat();
      std::unique_lock<std::mutex> lock(stop_mutex_);
      if (stop_.wait_for(lock, interval_, [this] { return stopping_; })) {
        return;
      }
    }
  }

  FrameChannel& channel_;
  SharedBlackbox& blackbox_;
  WorkerSessionConfig config_;
  const std::atomic<int>& published_state_;
  // The session's loop step counter. The helper thread copies it into every
  // heartbeat so the supervisor can tell a live helper thread from a live loop.
  const std::atomic<std::uint64_t>& published_progress_;
  // Set by the driver only while it is parked in Monitoring between Steps. Hot
  // reload reads this instead of the state byte so a stale heartbeat can never
  // swap the worker mid-transition.
  const std::atomic<bool>& safe_point_;
  std::mutex& send_mutex_;
  Duration interval_;
  std::thread thread_;
  std::mutex stop_mutex_;
  std::condition_variable stop_;
  bool stopping_ = false;
  std::atomic<std::uint32_t> last_sequence_{0};
  std::atomic<bool> broken_{false};
};

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

std::function<void(const StateSnapshot&)> MakeCompatFlagMirror(
    CompatFlags& flags, LegacyLog& log,
    std::function<std::string(const StateSnapshot&)> edge_profile,
    State initial_state) {
  // The callback is copied into the session config (and into the heartbeat
  // sender), so the transition cursor must be shared rather than captured by
  // value: every copy has to see the same previous state.
  const auto previous = std::make_shared<State>(initial_state);
  return [&flags, &log, edge_profile = std::move(edge_profile),
          previous](const StateSnapshot& snapshot) {
    const State from = *previous;
    *previous = snapshot.state;
    if (from == snapshot.state) return;
    const std::string profile =
        edge_profile == nullptr ? std::string{} : edge_profile(snapshot);
    // The legacy agent (focus-agent.ps1) consumes the ANSI/ACP form of these
    // paths: path::string() converts the wide compat directory with the ANSI code
    // page, so a non-ASCII config directory can mangle the two agent-arm payload
    // paths. A non-ASCII config directory is outside the legacy protocol this
    // payload serves; the native code would use Utf8Path, which the VBS-era agent
    // has no interface for.
    for (const FlagWrite& row : FlagsForStateTransition(
             from, snapshot.state, profile, flags.base_dir().string())) {
      // A failed write must not claim a log line: the legacy tools read the log
      // as the history of what actually reached the filesystem.
      if (!flags.WriteFlag(row.name, row.payload)) continue;
      log.Append(kWatchLog, "flag " + row.name +
                                (row.payload.empty() ? std::string{}
                                                     : " " + row.payload));
    }
  };
}

std::uint32_t RunWithStartupHeartbeats(FrameChannel& channel,
                                       SharedBlackbox& blackbox,
                                       WorkerSessionConfig config,
                                       const std::function<void()>& startup,
                                       bool* pipe_alive) {
  std::mutex send_mutex;
  std::atomic<int> published_state{static_cast<int>(State::Boot)};
  // Startup is not the worker loop yet: the sentinel tells the supervisor to
  // judge these heartbeats by sequence only.
  std::atomic<std::uint64_t> published_progress{kProgressNotStarted};
  std::atomic<bool> safe_point{false};
  HeartbeatSender heartbeat(channel, blackbox, config, published_state,
                            published_progress, safe_point, send_mutex);
  // The caller keeps the COM apartment: WPS automation objects are created on
  // this thread, and the heartbeat thread only touches the pipe and the box.
  // The sender must be stopped on every path, including a throwing startup.
  try {
    startup();
  } catch (...) {
    heartbeat.Stop();
    throw;
  }
  heartbeat.Stop();
  if (pipe_alive != nullptr) *pipe_alive = !heartbeat.broken();
  return heartbeat.last_sequence();
}

int RunWorkerSession(WorkerDriver& driver, FrameChannel& channel,
                     SharedBlackbox& blackbox, WorkerSessionConfig config,
                     std::string* error) {
  if (error != nullptr) error->clear();
  bool pipe_alive = !channel.broken();
  bool loss_reported = false;
  const auto report_loss = [&](const char* detail) {
    if (loss_reported) return;
    loss_reported = true;
    driver.HandleSupervisorLost(detail);
    if (error != nullptr) *error = std::string("supervisor lost: ") + detail;
  };

  driver.Start();
  if (config.inject_supervisor_loss) {
    // Spec 6.6: report a lost supervisor without breaking the pipe, so the
    // loss policy (continue by default) is exercised on a real machine.
    driver.HandleSupervisorLost("injected supervisor loss");
  }
  std::atomic<int> published_state{static_cast<int>(driver.snapshot().state)};
  // The loop's step counter; it advances only after Step() returns, so a
  // worker wedged inside a COM call keeps heartbeating a frozen value.
  std::atomic<std::uint64_t> published_progress{0};
  // Owned by the driver: it stores true only while it is parked in Monitoring
  // between Steps. The session only copies the flag into heartbeats, so hot
  // reload keys on the one instant a restart cannot drop a transition instead of
  // on the stale state byte or the microsecond between two Steps.
  std::atomic<bool> safe_point{false};
  driver.SetSafePointFlag(&safe_point);
  std::mutex send_mutex;
  if (!pipe_alive) report_loss(kLossDetail);
  // One heartbeat thread for the whole loop: WorkerDriver::Step() blocks in
  // WorkerRuntime until the poll interval or the next machine deadline, and a
  // long COM action inside it must not starve the supervisor. This loop adds
  // no sleep of its own.
  HeartbeatSender heartbeat(channel, blackbox, config, published_state,
                            published_progress, safe_point, send_mutex);
  bool external_stop_sent = false;
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
    if (heartbeat.broken() && !loss_reported) {
      pipe_alive = false;
      report_loss(kLossDetail);
    }
    // Spec 11.3: the legacy stop script deletes watcher.lock instead of sending
    // a frame. Latch it so the stop reason is journalled once.
    if (!external_stop_sent && config.external_stop &&
        config.external_stop()) {
      external_stop_sent = true;
      driver.RequestStop(kLegacyStopReason);
    }
    if (driver.done()) break;
    const bool keep_going = driver.Step();
    const StateSnapshot& snapshot = driver.snapshot();
    // Published after the step returns, never before: a blocking Step must not
    // look like loop progress. The driver already cleared its safe-point flag
    // when the Step returned, so no session store is needed.
    published_progress.fetch_add(1, std::memory_order_relaxed);
    // Every step, including the one that ends the run, so a final transition
    // into Stopping/SafeMode/Exited is still mirrored.
    if (config.on_step) config.on_step(snapshot);
    if (!keep_going) break;
    published_state.store(static_cast<int>(snapshot.state),
                          std::memory_order_relaxed);
  }

  // Stop the sender before the final Cleanup frame: the two share the send
  // mutex, and the Cleanup must carry the last sequence.
  heartbeat.Stop();
  // The safe-point atomic lives on this stack; detach it before the driver can
  // outlive the session.
  driver.SetSafePointFlag(nullptr);
  if (heartbeat.broken()) {
    pipe_alive = false;
    report_loss(kLossDetail);
  }
  // A worker that finished on its own still reports the clean exit so the
  // supervisor can tell a stop from a crash; a dead pipe has no one to tell.
  if (pipe_alive) {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (!channel.Send(MakeCleanup(heartbeat.last_sequence() + 1))) {
      if (error != nullptr) *error = "cleanup frame was not delivered";
    }
  }
  return 0;
}

} // namespace htmlppt

#endif // _WIN32
