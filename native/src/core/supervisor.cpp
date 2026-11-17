#include "core/supervisor.h"

#include <algorithm>
#include <utility>

namespace htmlppt {

SupervisorCore::SupervisorCore(SupervisorConfig config, Clock& clock)
    : config_(std::move(config)),
      clock_(clock),
      sequence_(clock_.now()),
      progress_(clock_.now()),
      budget_(config_.max_restarts, config_.restart_window),
      random_state_(config_.seed == 0 ? 0x9e3779b9u : config_.seed) {}

std::vector<SupervisorAction> SupervisorCore::Start() {
  if (worker_running_ || safe_mode_ || stopping_ ||
      restart_deadline_.has_value()) {
    return {};
  }
  worker_running_ = true;
  sequence_.reset(clock_.now());
  progress_.reset(clock_.now());
  return {{SupervisorActionKind::StartWorker, "start worker"}};
}

std::vector<SupervisorAction> SupervisorCore::Handle(
    const WorkerEvent& event) {
  if (safe_mode_) {
    if (event.kind == WorkerEventKind::StopRequested) {
      return {{SupervisorActionKind::StopComplete, "safe mode stop"}};
    }
    return {};
  }
  switch (event.kind) {
    case WorkerEventKind::Started:
      if (stopping_ || worker_running_ || restart_deadline_.has_value()) {
        return {};
      }
      worker_running_ = true;
      sequence_.reset(event.at);
      progress_.reset(event.at);
      return {};
    case WorkerEventKind::Heartbeat:
      if (!worker_running_ || stopping_) return {};
      sequence_.observe(event.sequence, event.at);
      if (event.progress.has_value()) {
        progress_.observe(*event.progress, event.at);
      }
      return {};
    case WorkerEventKind::Exited: {
      if (!worker_running_) return {};
      worker_running_ = false;
      if (stopping_) return {};
      auto actions = std::vector<SupervisorAction>{
          {SupervisorActionKind::RequestDump, "worker exited"}};
      auto scheduled = ScheduleRestart(event.at, "worker exited");
      actions.insert(actions.end(), scheduled.begin(), scheduled.end());
      return actions;
    }
    case WorkerEventKind::Hung:
      if (!worker_running_ || stopping_) return {};
      return HangActions(event.at, "worker hung");
    case WorkerEventKind::StopRequested:
      stopping_ = true;
      restart_deadline_.reset();
      if (worker_running_) {
        return {{SupervisorActionKind::TerminateWorker, "stop requested"}};
      }
      return {{SupervisorActionKind::StopComplete, "stop requested"}};
    case WorkerEventKind::CleanupDone:
      if (!stopping_) return {};
      worker_running_ = false;
      return {{SupervisorActionKind::StopComplete, "cleanup done"}};
  }
  return {};
}

std::vector<SupervisorAction> SupervisorCore::Tick() {
  const auto now = clock_.now();
  if (safe_mode_ || stopping_) return {};
  if (restart_deadline_.has_value()) {
    if (now < *restart_deadline_) return {};
    restart_deadline_.reset();
    sequence_.reset(now);
    progress_.reset(now);
    worker_running_ = true;
    ++restart_count_;
    return {{SupervisorActionKind::RestartWorker, "restart worker"}};
  }
  if (worker_running_) {
    // The helper thread keeps the sequence advancing while the loop is stuck in
    // a call, so progress that stops moving is the hang signal (I2). A missing
    // heartbeat still falls back to the sequence tracker.
    const bool sequence_stalled =
        sequence_.stalled(now, config_.heartbeat_timeout);
    const bool progress_stalled =
        progress_.stalled(now, config_.heartbeat_timeout);
    if (sequence_stalled || progress_stalled) {
      return HangActions(now, progress_stalled && !sequence_stalled
                                  ? "worker progress stalled"
                                  : "heartbeat stalled");
    }
  }
  return {};
}

std::optional<Duration> SupervisorCore::BackoffFor(int attempt) {
  if (config_.backoff.empty()) return std::nullopt;
  const std::size_t index = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(attempt, 0)),
      config_.backoff.size() - 1);
  Duration delay = config_.backoff[index];
  if (config_.jitter_ratio > 0.0) {
    const auto max_jitter = static_cast<long long>(
        static_cast<double>(delay.count()) * config_.jitter_ratio);
    if (max_jitter > 0) {
      const auto span = static_cast<std::uint64_t>(2 * max_jitter + 1);
      const auto offset =
          static_cast<long long>(NextRandom() % span) - max_jitter;
      delay += Duration{offset};
      if (delay < Duration{0}) delay = Duration{0};
    }
  }
  return delay;
}

std::uint32_t SupervisorCore::NextRandom() {
  random_state_ ^= random_state_ << 13;
  random_state_ ^= random_state_ >> 17;
  random_state_ ^= random_state_ << 5;
  return random_state_;
}

std::vector<SupervisorAction> SupervisorCore::EnterSafeMode(
    std::string detail) {
  safe_mode_ = true;
  worker_running_ = false;
  restart_deadline_.reset();
  std::vector<SupervisorAction> actions;
  actions.push_back({SupervisorActionKind::EnterSafeMode, std::move(detail)});
  if (config_.legacy_fallback) {
    actions.push_back(
        {SupervisorActionKind::RunLegacyFallback, "safe mode fallback"});
  }
  return actions;
}

std::vector<SupervisorAction> SupervisorCore::HangActions(ClockPoint at,
                                                          std::string detail) {
  worker_running_ = false;
  auto actions = std::vector<SupervisorAction>{
      {SupervisorActionKind::RequestDump, detail},
      {SupervisorActionKind::TerminateWorker, detail}};
  auto scheduled = ScheduleRestart(at, std::move(detail));
  actions.insert(actions.end(), scheduled.begin(), scheduled.end());
  return actions;
}

std::vector<SupervisorAction> SupervisorCore::ScheduleRestart(
    ClockPoint at, std::string detail) {
  if (!budget_.allow(at)) {
    return EnterSafeMode(std::move(detail) + ": restart budget exhausted");
  }
  const auto delay = BackoffFor(restart_count_);
  if (!delay.has_value()) {
    return EnterSafeMode(std::move(detail) + ": no backoff available");
  }
  budget_.record(at);
  restart_deadline_ = at + *delay;
  return {};
}

} // namespace htmlppt
