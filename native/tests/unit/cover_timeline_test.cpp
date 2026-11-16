#include "test_main.h"

#include <optional>

#include "core/clock.h"
#include "core/cover_timeline.h"

using htmlppt::ClockPoint;
using htmlppt::CoverPhase;
using htmlppt::CoverTick;
using htmlppt::CoverTimeline;
using htmlppt::CoverTiming;
using htmlppt::Duration;
using htmlppt::FakeClock;

HTMLPPT_TEST_CASE(cover_fades_in_and_reports_hold) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  CoverTick tick = timeline.Tick(start + Duration{150});
  HTMLPPT_CHECK_EQ(tick.opacity > 0.49 && tick.opacity < 0.51, true);
  HTMLPPT_CHECK(!tick.hold_reached);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeIn);

  tick = timeline.Tick(start + Duration{300});
  HTMLPPT_CHECK_EQ(tick.opacity, 1.0);
  HTMLPPT_CHECK(tick.hold_reached);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Hold);
}

HTMLPPT_TEST_CASE(cover_waits_for_minimum_hold_before_fading_out) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{300});
  timeline.FadeOut(start + Duration{500});
  CoverTick tick = timeline.Tick(start + Duration{900});
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Hold);
  HTMLPPT_CHECK_EQ(tick.opacity, 1.0);

  tick = timeline.Tick(start + Duration{1300});
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeOut);
  tick = timeline.Tick(start + Duration{1600});
  HTMLPPT_CHECK_EQ(tick.opacity, 0.0);
  HTMLPPT_CHECK(tick.done);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Idle);
}

HTMLPPT_TEST_CASE(cover_max_hold_forces_fade_out) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{2000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{300});
  CoverTick tick = timeline.Tick(start + Duration{2300});
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeOut);
  tick = timeline.Tick(start + Duration{2600});
  HTMLPPT_CHECK(tick.done);
}

HTMLPPT_TEST_CASE(cover_hide_resets_to_idle) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{150});
  timeline.Hide();
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Idle);
  HTMLPPT_CHECK_EQ(timeline.opacity(), 0.0);
  HTMLPPT_CHECK(!timeline.NextDeadline().has_value());
}

HTMLPPT_TEST_CASE(cover_next_deadline_during_fade_in) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  HTMLPPT_CHECK_EQ(timeline.NextDeadline(),
                   std::optional<ClockPoint>(start + Duration{300}));
}

HTMLPPT_TEST_CASE(cover_next_deadline_hold_uses_max_hold) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  const auto hold_start = start + Duration{300};
  timeline.Tick(hold_start);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Hold);
  HTMLPPT_CHECK_EQ(timeline.NextDeadline(),
                   std::optional<ClockPoint>(hold_start + Duration{10000}));
}

HTMLPPT_TEST_CASE(cover_next_deadline_hold_pending_uses_hold) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  const auto hold_start = start + Duration{300};
  timeline.Tick(hold_start);
  timeline.FadeOut(hold_start + Duration{100});
  HTMLPPT_CHECK_EQ(timeline.NextDeadline(),
                   std::optional<ClockPoint>(hold_start + Duration{1000}));
}

HTMLPPT_TEST_CASE(cover_next_deadline_hold_pending_capped_by_max_hold) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{5000}, Duration{2000}});

  timeline.FadeIn(start);
  const auto hold_start = start + Duration{300};
  timeline.Tick(hold_start);
  timeline.FadeOut(hold_start + Duration{100});
  HTMLPPT_CHECK_EQ(timeline.NextDeadline(),
                   std::optional<ClockPoint>(hold_start + Duration{2000}));
}

HTMLPPT_TEST_CASE(cover_next_deadline_during_fade_out) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{300});
  timeline.FadeOut(start + Duration{400});
  const auto fade_out_start = start + Duration{1300};
  timeline.Tick(fade_out_start);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeOut);
  HTMLPPT_CHECK_EQ(timeline.NextDeadline(),
                   std::optional<ClockPoint>(fade_out_start + Duration{300}));
}
