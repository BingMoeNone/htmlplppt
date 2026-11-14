#include "core/scheduling.h"

namespace htmlppt {
namespace {

struct Intervals {
  Duration far;
  Duration near;
  Duration at_target;
  Duration critical;
  Duration live;
  Duration stalled;
};

Intervals intervals_for(SchedulingProfile profile) {
  switch (profile) {
    case SchedulingProfile::Aggressive:
      return {.far = Duration{5},
              .near = Duration{1},
              .at_target = Duration{1},
              .critical = Duration{1},
              .live = Duration{1},
              .stalled = Duration{10}};
    case SchedulingProfile::Adaptive:
      return {.far = Duration{10},
              .near = Duration{2},
              .at_target = Duration{1},
              .critical = Duration{1},
              .live = Duration{1},
              .stalled = Duration{10}};
    case SchedulingProfile::Balanced:
      return {.far = Duration{10},
              .near = Duration{2},
              .at_target = Duration{2},
              .critical = Duration{2},
              .live = Duration{2},
              .stalled = Duration{20}};
    case SchedulingProfile::Safe:
      return {.far = Duration{20},
              .near = Duration{10},
              .at_target = Duration{5},
              .critical = Duration{5},
              .live = Duration{10},
              .stalled = Duration{50}};
  }
  return {.far = Duration{10},
          .near = Duration{2},
          .at_target = Duration{2},
          .critical = Duration{2},
          .live = Duration{2},
          .stalled = Duration{20}};
}

} // namespace

Duration poll_interval(State state, int distance_to_nearest_target,
                       SchedulingProfile profile, PowerSource power) {
  const Intervals intervals = intervals_for(effective_profile(profile, power));

  switch (state) {
    case State::Monitoring:
      if (distance_to_nearest_target == 0) {
        return intervals.at_target;
      }
      if (distance_to_nearest_target >= 1 &&
          distance_to_nearest_target <= 2) {
        return intervals.near;
      }
      // Negative distances are the sentinel for "no known target" and share
      // the far bucket with distances greater than two.
      return intervals.far;
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
      return intervals.far;
  }

  return intervals.far;
}

ThreadTier thread_tier(ThreadRole role) {
  switch (role) {
    case ThreadRole::ComState:
      return ThreadTier::Critical;
    case ThreadRole::Focus:
      return ThreadTier::Focus;
    case ThreadRole::UiOverlay:
      return ThreadTier::Ui;
    case ThreadRole::IpcHealth:
      return ThreadTier::Background;
    case ThreadRole::Supervisor:
      return ThreadTier::Supervisor;
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
