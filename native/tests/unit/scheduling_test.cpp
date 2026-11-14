#include "test_main.h"

#include "core/scheduling.h"

using htmlppt::Duration;
using htmlppt::PowerSource;
using htmlppt::SchedulingProfile;
using htmlppt::State;
using htmlppt::ThreadTier;

HTMLPPT_TEST_CASE(adaptive_intervals_follow_state_and_distance) {
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      10);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 2, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::CoverIn, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Live, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Faulted, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      10);
}

HTMLPPT_TEST_CASE(safe_profile_is_conservative) {
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 5, SchedulingProfile::Safe,
                    PowerSource::AC)
          .count(),
      20);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::CoverIn, 5, SchedulingProfile::Safe,
                    PowerSource::AC)
          .count(),
      5);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Live, 5, SchedulingProfile::Safe, PowerSource::AC)
          .count(),
      10);
}

HTMLPPT_TEST_CASE(battery_downgrades_aggressive_and_adaptive) {
  HTMLPPT_CHECK(
      effective_profile(SchedulingProfile::Aggressive, PowerSource::Battery) ==
      SchedulingProfile::Balanced);
  HTMLPPT_CHECK(
      effective_profile(SchedulingProfile::Adaptive, PowerSource::Battery) ==
      SchedulingProfile::Balanced);
  HTMLPPT_CHECK(
      effective_profile(SchedulingProfile::Safe, PowerSource::Battery) ==
      SchedulingProfile::Safe);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 5, SchedulingProfile::Aggressive,
                    PowerSource::Battery)
          .count(),
      10);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::CoverIn, 5, SchedulingProfile::Aggressive,
                    PowerSource::Battery)
          .count(),
      2);
}

HTMLPPT_TEST_CASE(thread_tiers_follow_criticality) {
  HTMLPPT_CHECK(thread_tier(State::CoverIn) == ThreadTier::Critical);
  HTMLPPT_CHECK(thread_tier(State::Monitoring) == ThreadTier::Focus);
  HTMLPPT_CHECK(thread_tier(State::Warming) == ThreadTier::Ui);
  HTMLPPT_CHECK(thread_tier(State::Faulted) == ThreadTier::Background);
}
