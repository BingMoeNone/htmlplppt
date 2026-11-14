#pragma once

#include "core/clock.h"
#include "core/types.h"

namespace htmlppt {

Duration poll_interval(State state, int distance_to_nearest_target,
                       SchedulingProfile profile, PowerSource power);

ThreadTier thread_tier(ThreadRole role);

SchedulingProfile effective_profile(SchedulingProfile requested,
                                    PowerSource power);

} // namespace htmlppt
