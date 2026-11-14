#include "core/worker_health.h"

namespace htmlppt {

void WorkerHealthMonitor::Register(ThreadRole role, ClockPoint now) {
  slots_.emplace(role, Slot{0, now});
}

void WorkerHealthMonitor::Beat(ThreadRole role, ClockPoint now) {
  const auto slot = slots_.find(role);
  if (slot == slots_.end()) return;
  ++slot->second.sequence;
  slot->second.last_beat = now;
}

std::vector<ThreadRole> WorkerHealthMonitor::Stale(ClockPoint now,
                                                   Duration timeout) const {
  std::vector<ThreadRole> stale;
  for (const auto& [role, slot] : slots_) {
    if (now - slot.last_beat > timeout) stale.push_back(role);
  }
  return stale;
}

bool WorkerHealthMonitor::Healthy(ClockPoint now, Duration timeout) const {
  return Stale(now, timeout).empty();
}

std::optional<ClockPoint> WorkerHealthMonitor::LastBeat(
    ThreadRole role) const {
  const auto slot = slots_.find(role);
  if (slot == slots_.end()) return std::nullopt;
  return slot->second.last_beat;
}

} // namespace htmlppt
