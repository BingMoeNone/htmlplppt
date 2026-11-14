#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "core/clock.h"

namespace htmlppt {

struct RetryConfig {
  int max_attempts = 3;
  Duration base_delay{200};
  Duration max_delay{5000};
  double jitter_ratio = 0.2;
  std::uint32_t seed = 0x9e3779b9u;
};

class RetryPolicy {
 public:
  explicit RetryPolicy(RetryConfig config);

  std::optional<Duration> next_delay(int attempt_index);
  void reset();

 private:
  std::uint32_t next_random();

  RetryConfig config_;
  std::uint32_t state_;
};

class RestartBudget {
 public:
  RestartBudget(int max_restarts, Duration window);

  bool allow(ClockPoint now);
  void record(ClockPoint now);

 private:
  int max_restarts_;
  Duration window_;
  std::deque<ClockPoint> restarts_;
};

} // namespace htmlppt
