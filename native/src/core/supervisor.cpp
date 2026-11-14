#include "core/supervisor.h"

#include <utility>

namespace htmlppt {

SupervisorCore::SupervisorCore(SupervisorConfig config, Clock& clock)
    : config_(std::move(config)),
      clock_(clock),
      sequence_(clock_.now()),
      budget_(config_.max_restarts, config_.restart_window),
      random_state_(config_.seed == 0 ? 0x9e3779b9u : config_.seed) {}

std::vector<SupervisorAction> SupervisorCore::Start() {
  if (worker_running_ || safe_mode_ || stopping_) return {};
  worker_running_ = true;
  sequence_.reset(clock_.now());
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
      if (stopping_) return {};
      worker_running_ = true;
      sequence_.reset(event.at);
      return {};
    case WorkerEventKind::Heartbeat:
      if (!worker_running_ || stopping_) return {};
      sequence_.observe(event.sequence, event.at);
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
    case WorkerEventKind::Hung: {
      if (stopping_) return {};
      worker_running_ = false;
      auto actions = std::vector<SupervisorAction>{
          {SupervisorActionKind::RequestDump, "worker hung"},
          {SupervisorActionKind::TerminateWorker, "worker hung"}};
      auto scheduled = ScheduleRestart(event.at, "worker hung");
      actions.insert(actions.end(), scheduled.begin(), scheduled.end());
      return actions;
    }
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
    worker_running_ = true;
    ++restart_count_;
    return {{SupervisorActionKind::RestartWorker, "restart worker"}};
  }
  if (worker_running_ &&
      sequence_.stalled(now, config_.heartbeat_timeout)) {
    worker_running_ = false;
    auto actions = std::vector<SupervisorAction>{
        {SupervisorActionKind::RequestDump, "heartbeat stalled"},
        {SupervisorActionKind::TerminateWorker, "heartbeat stalled"}};
    auto scheduled = ScheduleRestart(now, "heartbeat stalled");
    actions.insert(actions.end(), scheduled.begin(), scheduled.end());
    return actions;
  }
  return {};
}

std::optional<Duration> SupervisorCore::BackoffFor(int attempt) {
  if (attempt < 0 ||
      static_cast<std::size_t>(attempt) >= config_.backoff.size()) {
    return std::nullopt;
  }
  Duration delay = config_.backoff[static_cast<std::size_t>(attempt)];
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
