#include "test_main.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/presentation_host.h"
#include "core/types.h"
#include "worker_fakes.h"

using htmlppt::ClockPoint;
using htmlppt::Duration;
using htmlppt::HostStatus;
using htmlppt::State;
using htmlppt::WorkerHostConfig;
using htmlppt::test::HLiveAtSlide1;
using htmlppt::test::HTriggeredAtSlide1;
using htmlppt::test::MakeResolvedTarget;
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
  // CoverHold fires when the fade-in completes (CoverTimeline::Tick sets the
  // phase to Hold at that point); the hold duration only gates the fade-out.
  h.advance(h.host.cover.fade / 2 + h.host.cover.hold + Duration{1});
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
}

// Task-2 review: an action kind the runtime does not own must be visible in the
// journal instead of vanishing in a silent default branch. The remaining
// unowned kinds are the cleanup actions: StopHelperThreads and ReleaseLock both
// carry the stop detail, and only the journal can make that text observable.
HTMLPPT_TEST_CASE(worker_runtime_journals_actions_it_does_not_own) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> EdgeStarting
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
  // LaunchEdge belongs to this task now, so it launches instead of journaling
  // its profile string.
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
  h.runtime.RequestStop("test stop");
  h.runtime.Step();                             // cleanup actions, then Exited
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Exited);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "test stop"));
}

// Task 5: the show-start action focuses the WPS slideshow window for the
// app-supplied pid list and screen metrics. Core never queries the screen.
HTMLPPT_TEST_CASE(worker_runtime_focuses_wps_show_window) {
  WorkerHarness h;
  h.host.wps_pids = {77};
  h.rebuild();
  auto wps = MakeWindow(9, 77, "Slide Show");
  wps.class_name = "screenClass";
  h.windows.windows = {wps};
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();                             // ShowStarted -> FocusWps
  HTMLPPT_CHECK_EQ(h.windows.activate_calls.size(), 1u);
  HTMLPPT_CHECK_EQ(h.windows.activate_calls.back(), 9u);
}

// An empty pid list can never match a WPS window, so the runtime must not pay
// the controller's full focus timeout for a call that cannot succeed.
HTMLPPT_TEST_CASE(worker_runtime_skips_focus_wps_without_pids) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();                             // ShowStarted -> FocusWps skipped
  HTMLPPT_CHECK(h.windows.activate_calls.empty());
  // Only Step()'s poll sleep remains; FocusWps did not sleep to focus.timeout.
  HTMLPPT_CHECK_EQ(h.clock.sleeps().size(), 1u);
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

// B2 core: the page signals return by renaming the Edge window title. One
// request moves Live -> Returning and the state then stays Returning while the
// renamed title is still present (the machine ignores duplicates in Returning
// by design; the post-Faulted re-arm is covered by the next case).
HTMLPPT_TEST_CASE(worker_runtime_return_title_enters_and_stays_in_returning) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // edge launched
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.runtime.Step();                             // EdgeReady -> ShowExit
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 0);
  h.runtime.Step();                             // show ended -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-RETURN")};
  h.runtime.Step();                             // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.runtime.Step();                             // same title: no repeat
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
}

// The machine retries a return only by leaving Faulted on a fresh
// ReturnRequested, so the renamed title must be reportable again after the
// ReturnResume timeout. Without re-arming, Faulted would be a stuck state.
//
// The page keeps the renamed title, so the retry is rate-limited: the window
// opens after the Step that observed the fault, one Step just inside the
// boundary must not leave Faulted, and the Step on the boundary must retry.
//
// cover_max_hold is tuned below the 300 ms cover fade: at the default 10 s the
// frame after the advance emits CoverHold first (ObserveCover runs before Tick)
// and leaves Returning for ReturnCoverOut, so the ReturnResume timeout could
// never fire. Keeping the fade unfinished makes the timeout reachable at the
// runtime level, which is the path under test.
HTMLPPT_TEST_CASE(worker_runtime_reports_return_again_after_faulted) {
  // Pin the spec default so a change cannot hide behind the test's own use of
  // h.host.return_retry_interval.
  HTMLPPT_CHECK_EQ(WorkerHostConfig{}.return_retry_interval, Duration{2000});
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.config.cover_max_hold = Duration{100};
  h.plan.machine.targets = {MakeTarget(1, "t1", "p1")};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.rebuild();
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();                             // Warming -> Monitoring
  h.presentation.show.slide = 2;                // leave-forward trigger
  h.runtime.Step();                             // CoverIn + fade in
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> edge launched
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.runtime.Step();                             // EdgeReady -> ShowExit
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 0);
  h.runtime.Step();                             // show ended -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-RETURN")};
  h.runtime.Step();                             // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.advance(h.config.cover_max_hold + Duration{1});
  h.runtime.Step();                             // ReturnResume timeout -> Faulted
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Faulted);
  h.advance(h.host.return_retry_interval - Duration{1});
  h.runtime.Step();                             // one tick inside: no retry
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Faulted);
  h.advance(Duration{1});
  h.runtime.Step();                             // boundary reached: retry
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
}

// Task 5 main flow: the switch exits the WPS show when Edge is ready and
// resumes it at the target slide once the page reports the return title. Both
// actions run against the presentation host instead of only being journaled.
HTMLPPT_TEST_CASE(worker_runtime_exits_show_then_resumes_on_return) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // EdgeStarting + LaunchEdge
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.runtime.Step();                             // EdgeReady -> ShowExit + ExitShow
  HTMLPPT_CHECK_EQ(h.presentation.exit_show_calls, 1);
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 0);   // show ended
  h.runtime.Step();                             // ShowEnded -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.windows.windows = {MakeWindow(2, h.launcher.pid, "TEF-LIVE-RETURN")};
  h.runtime.Step();                             // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> ReturnCoverOut + ResumeShow
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ReturnCoverOut);
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.size(), 1u);
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.back(), 1);
}

// Ruling: a failed ExitShow is a transient failure in ShowExit, so the
// machine's existing ExitShow retry backoff runs and retries the same action.
HTMLPPT_TEST_CASE(worker_runtime_retries_failed_exit_show) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // EdgeStarting + LaunchEdge
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.presentation.fail_next_exit = true;
  h.runtime.Step();                             // EdgeReady -> ExitShow fails
  HTMLPPT_CHECK_EQ(h.presentation.exit_show_calls, 1);
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ShowExit);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "exit show failed"));
  h.advance(h.config.retry.max_delay);          // let the retry deadline pass
  h.runtime.Step();                             // Retry -> ExitShow succeeds
  HTMLPPT_CHECK_EQ(h.presentation.exit_show_calls, 2);
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 0);
  h.runtime.Step();                             // show ended -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
}

// Ruling: without this failure the machine's return-failure path is dead in
// production (CoverHold preempts the ReturnResume timeout at default timings),
// so a failed resume must report a Return failure. HandleReturnFailure journals
// both the "return failure" entry and its operator diagnostic, and the
// rate-limited retry keeps the state in Faulted instead of re-emitting on the
// same Step and hammering a wedged WPS.
HTMLPPT_TEST_CASE(worker_runtime_reports_return_failure_when_resume_show_fails) {
  WorkerHarness h = HLiveAtSlide1();
  h.windows.windows = {MakeWindow(2, h.launcher.pid, "TEF-LIVE-RETURN")};
  h.runtime.Step();                             // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.presentation.fail_next_resume = true;
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> ResumeShow fails
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.size(), 1u);
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.back(), 1);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "resume show failed"));
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "WPS resume failed"));
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Faulted);
}

// Task 6: a stop request must run the machine's cleanup batch exactly once and
// then feed CleanupDone, which no machine path emits by itself. The Step that
// runs the batch must reach Exited and every later Step must execute nothing.
HTMLPPT_TEST_CASE(worker_runtime_stop_mid_trigger_cleans_up_once) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> Edge launched
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
  h.runtime.RequestStop("test");
  h.runtime.Step();                             // cleanup batch + CleanupDone
  HTMLPPT_CHECK(h.overlay.hide_calls >= 1);     // CoverHide ran
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 1);   // KillEdge ran
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Exited);
  HTMLPPT_CHECK(h.runtime.done());
  const int hides = h.overlay.hide_calls;
  const std::size_t entries = h.runtime.journal().size();
  h.runtime.RequestStop("again");               // a second stop must not re-run
  HTMLPPT_CHECK(!h.runtime.Step());             // done: executes nothing
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 1);
  HTMLPPT_CHECK_EQ(h.overlay.hide_calls, hides);
  HTMLPPT_CHECK_EQ(h.runtime.journal().size(), entries);
}

// Task 6: the idle wait is bounded by the poll interval.
HTMLPPT_TEST_CASE(worker_runtime_step_sleeps_at_most_poll_interval) {
  WorkerHarness h;
  const ClockPoint before = h.clock.now();
  h.runtime.Step();
  HTMLPPT_CHECK(h.clock.sleeps().back() - before <=
                h.host.poll_interval + Duration{1});
}

// Task 6: the idle wait is bounded by the machine's next deadline too. A short
// CoverHold deadline must not be pushed out by a long poll interval, because the
// timeout is the machine's, not the runtime's.
HTMLPPT_TEST_CASE(worker_runtime_step_sleeps_until_the_next_deadline) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.config.cover_max_hold = Duration{50};
  h.host.poll_interval = Duration{10000};
  h.plan.machine.targets = {MakeTarget(1, "t1", "p1")};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.rebuild();
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();                             // Monitoring; full poll wait
  h.presentation.show.slide = 2;                // leave-forward trigger
  const ClockPoint before = h.clock.now();
  h.runtime.Step();                             // CoverIn: CoverHold due in 50 ms
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::CoverIn);
  HTMLPPT_CHECK_EQ(h.clock.sleeps().back() - before, h.config.cover_max_hold);
}

// Ruling: Unavailable and Failed are COM query failures exactly like Busy. A
// failed poll with active=false must not journal "show ended" and must not move
// prev_slide, or a wedged host would look like the end of the show.
HTMLPPT_TEST_CASE(worker_runtime_failed_poll_is_not_a_show_end) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();                             // Monitoring at slide 1
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().prev_slide, 1);
  const HostStatus failing[] = {HostStatus::Unavailable, HostStatus::Failed};
  for (const HostStatus status : failing) {
    h.presentation.show = MakeShowState(status, false, 0);
    h.runtime.Step();
    HTMLPPT_CHECK(!JournalContains(h.runtime.journal(), "show ended"));
    HTMLPPT_CHECK_EQ(h.runtime.snapshot().prev_slide, 1);
    HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  }
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "COM unavailable"));
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "COM failed"));
}

// Ruling: the journal is diagnostic state that a sustained failure poll must not
// grow without bound; the oldest entries are dropped once it is full. A poll
// while Faulted journals one entry per Step, so 600 of them leave the cap.
HTMLPPT_TEST_CASE(worker_runtime_journal_is_bounded) {
  WorkerHarness h = HLiveAtSlide1();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.windows.windows = {MakeWindow(2, h.launcher.pid, "TEF-LIVE-RETURN")};
  h.runtime.Step();                             // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.presentation.fail_next_resume = true;
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // failed resume -> Faulted
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Faulted);
  h.windows.windows.clear();                    // no return title to re-arm on
  h.presentation.show.status = HostStatus::Busy;
  for (int i = 0; i < 600; ++i) {
    h.runtime.Step();                           // "failure while faulted" each poll
  }
  // Pin the spec's cap so a change to the implementation constant cannot hide.
  HTMLPPT_CHECK_EQ(h.runtime.journal().size(), 500u);
  // The oldest entries went first, including the very first one.
  HTMLPPT_CHECK(!JournalContains(h.runtime.journal(), "state=Warming"));
}

// Task 6: an Edge that never reports the return title is killed at the live
// deadline; the machine returns through the cover instead of staying live.
// CoverHold arrives when the fade-in completes, so advancing fade + hold keeps
// the assertion on the state rather than on an exact tick.
HTMLPPT_TEST_CASE(worker_runtime_edge_live_timeout_removes_cover_and_resumes) {
  WorkerHarness h = HLiveAtSlide1();         // Live, Edge up, no return title ever arrives
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.advance(h.config.edge_max_live + Duration{1});
  h.runtime.Step();                          // EdgeLive timeout -> Returning (cover fades in)
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 1);
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                          // CoverHold -> ReturnCoverOut + ResumeShow
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ReturnCoverOut);
  HTMLPPT_CHECK_EQ(h.overlay.hide_calls, 0); // fade out, not a failure hide
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.size(), 1u);
  HTMLPPT_CHECK(h.presentation.resume_slides.back() == 1);
}
