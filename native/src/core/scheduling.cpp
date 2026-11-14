#include "core/scheduling.h"

namespace htmlppt {
namespace {

struct Intervals {
  Duration critical;
  Duration idle;
  Duration live;
  Duration stalled;
};

Intervals intervals_for(SchedulingProfile profile) {
  switch (profile) {
    case SchedulingProfile::Aggressive:
      return {Duration{1}, Duration{5}, Duration{1}, Duration{10}};
    case SchedulingProfile::Adaptive:
      return {Duration{1}, Duration{10}, Duration{1}, Duration{10}};
    case SchedulingProfile::Balanced:
      return {Duration{2}, Duration{10}, Duration{2}, Duration{20}};
    case SchedulingProfile::Safe:
      return {Duration{5}, Duration{20}, Duration{10}, Duration{50}};
  }
  return {Duration{10}, Duration{10}, Duration{10}, Duration{20}};
}

} // namespace

Duration poll_interval(State state, int distance_to_nearest_target,
                       SchedulingProfile profile, PowerSource power) {
  const Intervals intervals = intervals_for(effective_profile(profile, power));

  switch (state) {
    case State::Monitoring:
      if (distance_to_nearest_target >= 0 &&
          distance_to_nearest_target <= 2) {
        return intervals.critical;
      }
      return intervals.idle;
    case State::CoverIn:
    case State::EdgeStarting:
    case State::ShowExit:
    case State::Returning:
    case State::ReturnCoverOut:
      return intervals.critical;
    case State::Live:
      return intervals.live;
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
      return intervals.stalled;
    case State::Boot:
    case State::Warming:
    case State::DegradedNoSupervisor:
      return intervals.idle;
  }

  return intervals.idle;
}

ThreadTier thread_tier(State state) {
  switch (state) {
    case State::CoverIn:
    case State::EdgeStarting:
    case State::ShowExit:
    case State::Live:
    case State::Returning:
    case State::ReturnCoverOut:
      return ThreadTier::Critical;
    case State::Monitoring:
      return ThreadTier::Focus;
    case State::Warming:
    case State::Boot:
    case State::DegradedNoSupervisor:
      return ThreadTier::Ui;
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
      return ThreadTier::Background;
  }
  return ThreadTier::Background;
}

SchedulingProfile effective_profile(SchedulingProfile requested,
                                    PowerSource power) {
  if (power == PowerSource::Battery) {
    if (requested == SchedulingProfile::Aggressive ||
        requested == SchedulingProfile::Adaptive) {
      return SchedulingProfile::Balanced;
    }
  }
  return requested;
}

} // namespace htmlppt
