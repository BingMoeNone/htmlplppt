#include "test_main.h"

#include <string>
#include <string_view>
#include <vector>

#include "core/presentation_host.h"
#include "core/types.h"
#include "worker_fakes.h"

using htmlppt::Duration;
using htmlppt::HostStatus;
using htmlppt::State;
using htmlppt::test::HTriggeredAtSlide1;
using htmlppt::test::MakeShowState;
using htmlppt::test::MakeTarget;
using htmlppt::test::WorkerHarness;

namespace {

bool JournalContains(const std::vector<std::string>& journal,
                     std::string_view text) {
  for (const auto& entry : journal) {
    if (entry.find(text) != std::string::npos) return true;
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(worker_runtime_starts_show_and_tracks_slide_changes) {
  WorkerHarness h;                       // fakes from worker_fakes.h
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Warming);   // Start only
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  h.presentation.show.slide = 2;
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().prev_slide, 2);
}

// A busy COM poll reports a transient COM failure and keeps the last good show
// sample, so it can never look like a slide change or a show end.
HTMLPPT_TEST_CASE(worker_runtime_reports_com_busy_without_show_ended) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start(); h.runtime.Step();
  h.presentation.show.status = HostStatus::Busy;
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  HTMLPPT_CHECK(h.presentation.exit_show_calls == 0);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "COM busy"));
}

HTMLPPT_TEST_CASE(worker_runtime_fades_cover_in_holds_and_reports_done) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.plan.machine.targets = {MakeTarget(1, "t1", "p1")};   // target slide 1
  h.rebuild();                                            // re-create runtime with the new plan
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start(); h.runtime.Step();
  h.presentation.show.slide = 2;                          // leave-forward
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.overlay.show_calls, 1);              // CoverFadeIn ran
  HTMLPPT_CHECK(h.overlay.opacity <= 0.01);               // fade starts transparent
  h.advance(h.host.cover.fade / 2);
  h.runtime.Step();                                       // ObserveCover ticks the fade
  HTMLPPT_CHECK(h.overlay.opacity > 0.0 && h.overlay.opacity < 1.0);
  // CoverHold fires only after fade + hold, never after hold alone.
  h.advance(h.host.cover.fade / 2 + h.host.cover.hold + Duration{1});
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
}

// Task-2 review: an action kind the runtime does not own must be visible in the
// journal instead of vanishing in a silent default branch.
HTMLPPT_TEST_CASE(worker_runtime_journals_actions_it_does_not_own) {
  WorkerHarness h = HTriggeredAtSlide1();
  // FocusWps is owned by a later task; show-start must still journal it.
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "focus WPS"));
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
  // LaunchEdge is Task 4's; until then its profile is journaled.
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "p1"));
}
