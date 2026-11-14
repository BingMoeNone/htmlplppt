#include "core/retry_policy.h"

#include <algorithm>

namespace htmlppt {

RetryPolicy::RetryPolicy(RetryConfig config)
    : config_(config),
      state_(config.seed == 0 ? 0x9e3779b9u : config.seed) {}

std::uint32_t RetryPolicy::next_random() {
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  return state_;
}

std::optional<Duration> RetryPolicy::next_delay(int attempt_index) {
  if (attempt_index < 0 || attempt_index >= config_.max_attempts) {
    return std::nullopt;
  }

  const int shift = std::min(attempt_index, 20);
  Duration delay = config_.base_delay * (1LL << shift);
  if (delay > config_.max_delay) {
    delay = config_.max_delay;
  }

  if (config_.jitter_ratio > 0.0) {
    const auto max_jitter =
        static_cast<long long>(delay.count() * config_.jitter_ratio);
    if (max_jitter > 0) {
      const auto span = static_cast<std::uint64_t>(2 * max_jitter + 1);
      const auto offset =
          static_cast<long long>(next_random() % span) - max_jitter;
      delay += Duration{offset};
      if (delay < Duration{0}) {
        delay = Duration{0};
      }
    }
  }

  return delay;
}

void RetryPolicy::reset() {
  state_ = config_.seed == 0 ? 0x9e3779b9u : config_.seed;
}

RestartBudget::RestartBudget(int max_restarts, Duration window)
    : max_restarts_(max_restarts), window_(window) {}

bool RestartBudget::allow(ClockPoint now) {
  while (!restarts_.empty() && now - restarts_.front() > window_) {
    restarts_.pop_front();
  }
  return static_cast<int>(restarts_.size()) < max_restarts_;
}

void RestartBudget::record(ClockPoint now) {
  restarts_.push_back(now);
}

} // namespace htmlppt
