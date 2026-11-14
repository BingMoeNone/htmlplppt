#include "test_main.h"

#include "core/scheduling.h"

using htmlppt::Duration;
using htmlppt::PowerSource;
using htmlppt::SchedulingProfile;
using htmlppt::State;
using htmlppt::ThreadRole;
using htmlppt::ThreadTier;

HTMLPPT_TEST_CASE(adaptive_intervals_follow_state_and_distance) {
  // A negative distance is the sentinel for "no known target"; Monitoring
  // treats it like any other far distance.
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 0, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 1, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      2);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 2, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      2);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 3, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      10);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, -1, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      10);
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
      poll_interval(State::Monitoring, 2, SchedulingProfile::Safe,
                    PowerSource::AC)
          .count(),
      10);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 0, SchedulingProfile::Safe,
                    PowerSource::AC)
          .count(),
      5);
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

HTMLPPT_TEST_CASE(thread_tiers_follow_role) {
  HTMLPPT_CHECK(thread_tier(ThreadRole::ComState) == ThreadTier::Critical);
  HTMLPPT_CHECK(thread_tier(ThreadRole::Focus) == ThreadTier::Focus);
  HTMLPPT_CHECK(thread_tier(ThreadRole::UiOverlay) == ThreadTier::Ui);
  HTMLPPT_CHECK(thread_tier(ThreadRole::IpcHealth) == ThreadTier::Background);
  HTMLPPT_CHECK(thread_tier(ThreadRole::Supervisor) == ThreadTier::Supervisor);
}
