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
using htmlppt::test::MakeWindow;
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
  // LaunchEdge belongs to this task now, so it launches instead of journaling
  // its profile string.
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
}

// Task 4: the cover hold launches Edge for the target's profile; once the Edge
// window is visible the runtime focuses it and reports readiness.
HTMLPPT_TEST_CASE(worker_runtime_launches_edge_then_reports_ready) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> EdgeStarting + LaunchEdge
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
  HTMLPPT_CHECK_EQ(h.launcher.last_spec.profile_dir, h.host.temp_dir / "p1");
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.runtime.Step();                             // EdgeReady -> ShowExit
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ShowExit);
}

HTMLPPT_TEST_CASE(worker_runtime_treats_dead_edge_as_closed_before_ready) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // Edge launched, waiting for ready
  h.launcher.set_alive(false);
  h.runtime.Step();                             // EdgeClosed -> switch failure
  HTMLPPT_CHECK_EQ(h.overlay.hide_calls, 1);    // switch failure hides the cover
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
}

// A rejected launch is a transient Edge failure: the runtime stores no pid and
// the machine's existing retry policy owns the retry.
HTMLPPT_TEST_CASE(worker_runtime_retries_after_edge_launch_failure) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.launcher.fail_next = true;
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // LaunchEdge fails -> transient
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "edge launch failed"));
  h.advance(h.config.retry.max_delay);          // let the retry deadline pass
  h.runtime.Step();                             // Retry -> LaunchEdge succeeds
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 2);
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.runtime.Step();                             // EdgeReady -> ShowExit
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ShowExit);
}
