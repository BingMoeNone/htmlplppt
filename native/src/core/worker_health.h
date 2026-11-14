#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "core/clock.h"
#include "core/types.h"

namespace htmlppt {

class WorkerHealthMonitor {
 public:
  void Register(ThreadRole role, ClockPoint now);
  void Beat(ThreadRole role, ClockPoint now);
  std::vector<ThreadRole> Stale(ClockPoint now, Duration timeout) const;
  bool Healthy(ClockPoint now, Duration timeout) const;
  std::optional<ClockPoint> LastBeat(ThreadRole role) const;
  std::size_t registered() const { return slots_.size(); }

 private:
  struct Slot {
    std::uint64_t sequence = 0;
    ClockPoint last_beat{};
  };
  std::map<ThreadRole, Slot> slots_;
};

} // namespace htmlppt
