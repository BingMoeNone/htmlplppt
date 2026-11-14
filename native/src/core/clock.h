#pragma once

#include <chrono>
#include <thread>
#include <vector>

namespace htmlppt {

using ClockPoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::milliseconds;

class Clock {
 public:
  virtual ~Clock() = default;
  virtual ClockPoint now() const = 0;
  virtual void sleep_until(ClockPoint deadline) = 0;
};

class SteadyClock final : public Clock {
 public:
  ClockPoint now() const override {
    return std::chrono::steady_clock::now();
  }

  void sleep_until(ClockPoint deadline) override {
    if (deadline > now()) {
      std::this_thread::sleep_until(deadline);
    }
  }
};

class FakeClock final : public Clock {
 public:
  explicit FakeClock(ClockPoint start = ClockPoint(Duration{0}))
      : now_(start) {}

  ClockPoint now() const override {
    return now_;
  }

  void sleep_until(ClockPoint deadline) override {
    if (deadline > now_) {
      now_ = deadline;
      sleeps_.push_back(deadline);
    }
  }

  void advance(Duration delta) {
    if (delta.count() > 0) {
      now_ += delta;
    }
  }

  const std::vector<ClockPoint>& sleeps() const {
    return sleeps_;
  }

 private:
  ClockPoint now_;
  std::vector<ClockPoint> sleeps_;
};

} // namespace htmlppt
