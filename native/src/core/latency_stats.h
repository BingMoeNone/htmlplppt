#pragma once

#include <cstddef>
#include <vector>

#include "core/clock.h"

namespace htmlppt {

// Bounded latency histogram for the --benchmark output. Samples are kept in a
// ring (newest win) so a long run cannot grow memory without bound; percentiles
// are computed on a sorted copy at report time, which only happens on exit.
class LatencyStats {
 public:
  explicit LatencyStats(std::size_t capacity = 4096);

  void Add(Duration latency);
  void Clear();

  std::size_t count() const { return seen_; }
  Duration P50() const;
  Duration P99() const;
  Duration Max() const;

 private:
  std::size_t capacity_;
  std::size_t seen_ = 0;
  std::size_t next_ = 0;
  std::vector<Duration> samples_;
};

} // namespace htmlppt
