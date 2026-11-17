#include "core/latency_stats.h"

#include <algorithm>
#include <utility>

namespace htmlppt {
namespace {

Duration Percentile(std::vector<Duration> samples, double fraction) {
  if (samples.empty()) return Duration{0};
  std::sort(samples.begin(), samples.end());
  if (fraction <= 0.0) return samples.front();
  if (fraction >= 1.0) return samples.back();
  const auto index = static_cast<std::size_t>(
      fraction * static_cast<double>(samples.size() - 1) + 0.5);
  return samples[std::min(index, samples.size() - 1)];
}

} // namespace

LatencyStats::LatencyStats(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {
  samples_.reserve(capacity_);
}

void LatencyStats::Add(Duration latency) {
  if (samples_.size() < capacity_) {
    samples_.push_back(latency);
  } else {
    samples_[next_] = latency;
    next_ = (next_ + 1) % capacity_;
  }
  ++seen_;
}

void LatencyStats::Clear() {
  samples_.clear();
  seen_ = 0;
  next_ = 0;
}

Duration LatencyStats::P50() const { return Percentile(samples_, 0.50); }

Duration LatencyStats::P99() const { return Percentile(samples_, 0.99); }

Duration LatencyStats::Max() const {
  if (samples_.empty()) return Duration{0};
  return *std::max_element(samples_.begin(), samples_.end());
}

} // namespace htmlppt
