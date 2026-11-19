#include "test_main.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/presentation_host.h"
#include "core/types.h"
#include "worker_fakes.h"

using htmlppt::ClockPoint;
using htmlppt::Duration;
using htmlppt::FocusStrategy;
using htmlppt::HostStatus;
using htmlppt::State;
using htmlppt::ScreenMetrics;
using htmlppt::WindowId;
using htmlppt::WindowInfo;
using htmlppt::WindowRect;
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

// A WPS slideshow window the focus scorer accepts. The rect matters: the focus
// layer only targets a window that covers 90% of the reference screen.
WindowInfo MakeWpsShowWindow(WindowId id, std::uint32_t pid, int width,
                             int height) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = "screenClass";
  window.title = "Slide Show";
  window.rect = WindowRect{0, 0, width, height};
  window.visible = true;
  window.ws_popup = true;
  window.has_min_box = false;
  window.has_max_box = false;
  return window;
}

// Drives one forward trigger from Monitoring to Returning, where the next cover
// hold emits ResumeShow + FocusWps. The caller seeds plan/config/host and
// rebuilds the runtime first, then finishes the return with one more cover hold.
void ReachReturning(WorkerHarness& h, WindowInfo wps) {
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.Step();  // Warming -> Monitoring
  h.presentation.show.slide = 2;
  h.Step();  // leave-forward -> CoverIn
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // CoverHold -> EdgeStarting + LaunchEdge
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
  h.windows.windows = {MakeWindow(1, h.launcher.pid, "TEF-LIVE-STAGE")};
  h.Step();  // EdgeReady -> ShowExit
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ShowExit);
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 0);
  h.Step();  // ShowEnded -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.windows.windows = {MakeWindow(2, h.launcher.pid, "TEF-LIVE-RETURN"),
                       std::move(wps)};
  h.Step();  // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
}

// Completes the return cover cycle: the machine is back in Monitoring while the
// runtime still carries whatever the finished target applied.
void ReturnToMonitoring(WorkerHarness& h) {
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // CoverHold -> ReturnCoverOut
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ReturnCoverOut);
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // hold done -> cover fade-out
  h.advance(h.host.cover.fade + Duration{1});
  h.Step();  // fade-out done -> CoverDone -> Monitoring
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
}

// A show restart is a show start with no target: its FocusWps must run on the
// host baseline, not on the values the previous target left behind.
void RestartShow(WorkerHarness& h) {
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 0);
  h.Step();  // show ended while monitoring
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Step();  // ShowStarted -> FocusWps
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

// Spec 3.6: a successful COM poll must reset the error window. Two isolated
// Busy samples far apart are not a contiguous COM outage; without the healthy
// signal the machine measures the window from the first error and the second
// isolated failure would enter SafeMode.
HTMLPPT_TEST_CASE(worker_runtime_isolated_com_error_does_not_arm_window) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start(); h.runtime.Step();                  // Monitoring
  h.presentation.show.status = HostStatus::Busy;
  h.runtime.Step();                             // first COM error
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  h.presentation.show.status = HostStatus::Ok;
  h.runtime.Step();                             // healthy poll resets the window
  h.advance(Duration{3000});
  h.presentation.show.status = HostStatus::Busy;
  h.runtime.Step();                             // second, isolated COM error
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  HTMLPPT_CHECK(!h.runtime.snapshot().safe_mode);
}

// A contiguous outage is still structural: repeated Busy polls across more than
// 2 s with no successful sample in between must reach SafeMode.
HTMLPPT_TEST_CASE(worker_runtime_continuous_com_error_reaches_safe_mode) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start(); h.runtime.Step();                  // Monitoring
  h.presentation.show.status = HostStatus::Busy;
  h.runtime.Step();                             // window opens
  h.advance(Duration{1000});
  h.runtime.Step();                             // still inside the window
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  h.advance(Duration{1000});
  h.runtime.Step();                             // window expired -> SafeMode
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::SafeMode);
  HTMLPPT_CHECK(h.runtime.snapshot().safe_mode);
}

// Warming owns the same COM error window as Monitoring: one Busy poll right
// after Start must not skip the window and jump to SafeMode. The worker keeps
// polling, and only a contiguous outage longer than the window is structural.
HTMLPPT_TEST_CASE(worker_runtime_warming_com_error_keeps_polling) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Busy, false, 0);
  h.Start();
  h.runtime.Step();                             // first COM error in Warming
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Warming);
  HTMLPPT_CHECK(!h.runtime.snapshot().safe_mode);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "COM busy"));
  h.advance(Duration{1000});
  h.runtime.Step();                             // still inside the window
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Warming);
  h.advance(Duration{1000});
  h.runtime.Step();                             // window expired -> SafeMode
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::SafeMode);
  HTMLPPT_CHECK(h.runtime.snapshot().safe_mode);
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

// A failed ResumeShow must keep Edge alive: the still-renamed title is the only
// signal that can retry the return. Closing Edge in the same action batch used
// to clear edge_pid_/return_sent_, so Faulted could never be left again and the
// operator had to recover by hand.
HTMLPPT_TEST_CASE(worker_runtime_keeps_edge_for_retry_after_failed_resume) {
  WorkerHarness h = HLiveAtSlide1();
  h.windows.windows = {MakeWindow(2, h.launcher.pid, "TEF-LIVE-RETURN")};
  h.runtime.Step();                             // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.presentation.fail_next_resume = true;
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // ResumeShow fails -> Faulted
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Faulted);
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 0);   // Edge is still there to retry
  h.advance(h.host.return_retry_interval - Duration{1});
  h.runtime.Step();                             // one tick inside: no retry
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Faulted);
  h.advance(Duration{1});
  h.runtime.Step();                             // boundary: title retries return
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.size(), 1u);
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 0);
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

// Edge must open the staged copy when the target asks for staging, so a deck
// folder can be handed over with only the source assets.
HTMLPPT_TEST_CASE(worker_runtime_stages_html_for_a_file_mode_target) {
  WorkerHarness h;
  const auto root =
      std::filesystem::temp_directory_path() / "htmlppt-runtime-stage";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  const auto source = root / "source.html";
  {
    std::ofstream out(source, std::ios::binary);
    out << "<html>live</html>";
  }

  h.plan.machine.targets = {MakeTarget(1, "t1", "p1")};
  auto resolved = MakeResolvedTarget(1, 1, "t1", "p1");
  resolved.html_path = source.string();
  resolved.stage_name = "page1.html";
  resolved.assets_mode = "file";
  h.plan.targets = {resolved};
  h.host.temp_dir = root / "work";
  h.config.startup_ignore = Duration{0};
  h.rebuild();
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();
  h.presentation.show.slide = 2;
  h.runtime.Step();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();  // CoverHold -> LaunchEdge with the staged path

  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
  const auto staged = root / "work" / "stage" / "page1.html";
  HTMLPPT_CHECK(std::filesystem::is_regular_file(staged));
  HTMLPPT_CHECK(h.launcher.last_spec.url.find("page1.html") != std::string::npos);
  std::filesystem::remove_all(root, error);
}
// While a show is being watched the loop polls faster, so a slide change is
// noticed within a frame budget rather than up to a full idle interval later.
HTMLPPT_TEST_CASE(worker_runtime_polls_actively_while_monitoring) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();  // Warming -> Monitoring
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  const ClockPoint before = h.clock.now();
  h.runtime.Step();
  HTMLPPT_CHECK(!h.clock.sleeps().empty());
  HTMLPPT_CHECK_EQ(h.clock.sleeps().back() - before,
                   h.host.active_poll_interval);
}

HTMLPPT_TEST_CASE(worker_runtime_default_poll_intervals) {
  HTMLPPT_CHECK_EQ(WorkerHostConfig{}.poll_interval, Duration{100});
  HTMLPPT_CHECK_EQ(WorkerHostConfig{}.active_poll_interval, Duration{20});
}
// Cleanup steps must be individually identifiable in the journal; two placeholder
// actions used to journal the same event detail, producing duplicate lines.
HTMLPPT_TEST_CASE(worker_runtime_cleanup_journals_distinct_steps) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();  // Warming -> Monitoring
  h.runtime.RequestStop("supervisor stop");
  h.runtime.Step();  // executes the cleanup batch

  const auto& journal = h.runtime.journal();
  const auto count = [&journal](const std::string& needle) {
    return std::count_if(journal.begin(), journal.end(), [&](const auto& line) {
      return line.find(needle) != std::string::npos;
    });
  };
  HTMLPPT_CHECK_EQ(count("cleanup start"), 1);
  HTMLPPT_CHECK_EQ(count("cleanup: helper threads"), 1);
  HTMLPPT_CHECK_EQ(count("cleanup: release the watcher lock"), 1);
  // Both steps still name the cause, but under distinct labels: two identical
  // "stop requested" lines used to make the log look like a double stop.
  HTMLPPT_CHECK_EQ(count("stop requested: supervisor stop"), 2);
}
// Spec 11.2: SafeMode must hide the cover, leave the slideshow and hand WPS back
// to the operator, not leave a full-screen show behind a hidden cover.
HTMLPPT_TEST_CASE(worker_runtime_safe_mode_restores_the_operator_view) {
  WorkerHarness h;
  h.host.wps_pids = {42};
  WindowInfo wps_window;
  wps_window.id = 7;
  wps_window.pid = 42;
  wps_window.class_name = "screenClass";
  wps_window.title = "deck - WPS Office";
  wps_window.visible = true;
  wps_window.ws_popup = true;
  wps_window.has_min_box = false;
  wps_window.has_max_box = false;
  h.windows.windows = {wps_window};
  h.rebuild();

  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();  // Warming -> Monitoring

  h.presentation.show = MakeShowState(HostStatus::Unavailable, false, 0);
  for (int i = 0; i < 40 && h.runtime.snapshot().state != State::SafeMode; ++i) {
    h.advance(Duration{100});
    h.runtime.Step();
  }

  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::SafeMode);
  HTMLPPT_CHECK_EQ(h.presentation.exit_show_calls, 1);
  HTMLPPT_CHECK(!h.windows.activate_calls.empty());
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "safe mode"));
}
// Spec 6.6: edge-slow models a cold start whose window appears late. Readiness
// must be held back, and the machine's own deadline must stay in charge.
HTMLPPT_TEST_CASE(worker_runtime_edge_slow_fault_delays_readiness) {
  WorkerHarness h;
  h.host.faults.edge_slow = true;
  h.config.startup_ignore = Duration{0};
  h.plan.machine.targets = {MakeTarget(1, "t1", "p1")};
  auto resolved = MakeResolvedTarget(1, 1, "t1", "p1");
  resolved.html_path = "/tmp/page.html";
  h.plan.targets = {resolved};
  h.rebuild();

  // The fake launcher hands out pid 500; a matching visible Edge window means
  // readiness depends only on the injected delay, not on window discovery.
  WindowInfo edge_window;
  edge_window.id = 900;
  edge_window.pid = 500;
  edge_window.visible = true;
  edge_window.rect = WindowRect{0, 0, 1920, 1080};
  h.windows.windows = {edge_window};

  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();
  h.presentation.show.slide = 2;
  h.runtime.Step();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();  // launches Edge, readiness held for 3 s
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);

  h.advance(Duration{1000});
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);

  h.advance(Duration{2500});
  h.runtime.Step();  // past the injected window: EdgeReady -> ShowExit
  h.presentation.show = MakeShowState(HostStatus::Ok, false, 2);
  h.runtime.Step();  // ShowEnded -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
}
HTMLPPT_TEST_CASE(worker_runtime_records_latency_samples) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  for (int i = 0; i < 5; ++i) h.runtime.Step();
  HTMLPPT_CHECK(h.runtime.step_stats().count() >= 5);
  HTMLPPT_CHECK(h.runtime.com_stats().count() >= 5);
}
HTMLPPT_TEST_CASE(worker_runtime_handles_resume_and_display_change) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.runtime.Step();  // Monitoring

  h.advance(Duration{5000});
  h.overlay.resume_event = true;
  h.runtime.Step();
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "power resume"));

  ScreenMetrics metrics;
  metrics.width = 1280;
  metrics.height = 720;
  h.overlay.display_change = metrics;
  h.runtime.Step();
  HTMLPPT_CHECK(
      JournalContains(h.runtime.journal(), "display change: 1280x720"));
}

HTMLPPT_TEST_CASE(worker_runtime_injects_resume_and_display_faults) {
  WorkerHarness h;
  h.host.faults.sleep_resume = true;
  h.host.faults.multi_monitor = true;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.rebuild();
  h.Start();
  h.runtime.Step();
  HTMLPPT_CHECK(
      JournalContains(h.runtime.journal(), "fault injected: sleep-resume"));
  HTMLPPT_CHECK(
      JournalContains(h.runtime.journal(), "fault injected: multi-monitor"));
}

// focus.require=false means "do not force the WPS window": the FocusWps action
// must not activate anything at all. The resume still proves the action fired.
HTMLPPT_TEST_CASE(worker_runtime_focus_require_false_skips_wps_activation) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.focus_require = false;
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.host.wps_pids = {77};
  h.rebuild();

  ReachReturning(h, MakeWpsShowWindow(9, 77, 1920, 1080));
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // CoverHold -> ResumeShow + skipped FocusWps
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ReturnCoverOut);
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.size(), std::size_t{1});
  for (const auto id : h.windows.activate_calls) {
    HTMLPPT_CHECK(id != WindowId{9});
  }
}

// require=true keeps the normal activation, and the target's focus.strategy
// must reach the focus controller instead of the global Auto chain.
HTMLPPT_TEST_CASE(worker_runtime_applies_per_target_focus_strategy) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.focus_strategy = "switch";
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.host.wps_pids = {77};
  h.rebuild();

  ReachReturning(h, MakeWpsShowWindow(9, 77, 1920, 1080));
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // CoverHold -> ResumeShow + FocusWps
  bool activated = false;
  for (const auto& call : h.windows.activate_with_calls) {
    if (call.first != WindowId{9}) continue;
    activated = true;
    HTMLPPT_CHECK(call.second == FocusStrategy::Switch);
  }
  HTMLPPT_CHECK(activated);
}

// The target's focus.timeout bounds the FocusWps attempt. With the idle poll
// disabled the fake clock only moves inside the focus controller, so the Step's
// elapsed time is the applied timeout.
HTMLPPT_TEST_CASE(worker_runtime_applies_per_target_focus_timeout) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.focus_timeout = Duration{250};
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.host.wps_pids = {77};
  h.host.poll_interval = Duration{0};
  h.rebuild();

  // Only the return Edge window exists, so FocusWps cannot match and exhausts
  // exactly its timeout before giving up.
  ReachReturning(h, MakeWpsShowWindow(9, 77, 1920, 1080));
  h.windows.windows = {MakeWindow(2, h.launcher.pid, "TEF-LIVE-RETURN")};
  const ClockPoint before = h.clock.now();
  const Duration cover_wait =
      h.host.cover.fade + h.host.cover.hold + Duration{1};
  h.advance(cover_wait);
  h.Step();  // CoverHold -> FocusWps times out after the target's 250 ms
  HTMLPPT_CHECK_EQ(h.clock.now() - before, cover_wait + Duration{250});
}

// A target display override replaces the host screen for the show-window
// scorer: this window is too small for the 1920x1080 host screen but big enough
// for the target's 640x360 monitor.
HTMLPPT_TEST_CASE(worker_runtime_applies_per_target_screen_on_trigger) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.screen_width = 640;
  target.screen_height = 360;
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.host.wps_pids = {77};
  h.host.screen = ScreenMetrics{1920, 1080};
  h.rebuild();

  ReachReturning(h, MakeWpsShowWindow(9, 77, 600, 330));
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // CoverHold -> FocusWps with the target's screen
  bool activated = false;
  for (const auto id : h.windows.activate_calls) {
    if (id == WindowId{9}) activated = true;
  }
  HTMLPPT_CHECK(activated);
}

// Without a display override the same window is rejected by the host screen, so
// the previous test's activation really comes from the per-target value.
HTMLPPT_TEST_CASE(worker_runtime_keeps_the_host_screen_without_an_override) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.host.wps_pids = {77};
  h.host.screen = ScreenMetrics{1920, 1080};
  h.rebuild();

  ReachReturning(h, MakeWpsShowWindow(9, 77, 600, 330));
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // CoverHold -> FocusWps cannot match the 1920x1080 screen
  for (const auto id : h.windows.activate_calls) {
    HTMLPPT_CHECK(id != WindowId{9});
  }
}

// defaults.focus.require=false reaches the show-start focus through the host
// config: no target exists for slide 0, so the plan entry cannot carry it.
HTMLPPT_TEST_CASE(worker_runtime_show_start_honours_the_default_focus_require) {
  WorkerHarness h;
  h.host.focus_require = false;
  h.host.wps_pids = {77};
  h.rebuild();
  h.windows.windows = {MakeWpsShowWindow(9, 77, 1920, 1080)};
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.Step();  // ShowStarted -> FocusWps, skipped by the default opt-out
  HTMLPPT_CHECK(h.windows.activate_calls.empty());
  HTMLPPT_CHECK(h.windows.activate_with_calls.empty());
}

// SafeMode hands the desktop back to the operator: the emergency restore focus
// must run even when the target opted out of routine focus.
HTMLPPT_TEST_CASE(worker_runtime_focus_require_false_still_restores_in_safe_mode) {
  WorkerHarness h;
  h.host.focus_require = false;
  h.host.wps_pids = {77};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.focus_require = false;
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.rebuild();
  h.windows.windows = {MakeWpsShowWindow(9, 77, 1920, 1080)};

  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.Step();  // Monitoring; the routine show-start focus is opted out
  HTMLPPT_CHECK(h.windows.activate_calls.empty());

  h.presentation.show = MakeShowState(HostStatus::Unavailable, false, 0);
  for (int i = 0; i < 40 && h.runtime.snapshot().state != State::SafeMode; ++i) {
    h.advance(Duration{100});
    h.Step();
  }
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::SafeMode);
  bool activated = false;
  for (const auto id : h.windows.activate_calls) {
    if (id == WindowId{9}) activated = true;
  }
  HTMLPPT_CHECK(activated);
}

// Switch-failure recovery is the same emergency hand-back, and it must not
// inherit the in-flight target's focus strategy: the baseline chain applies.
HTMLPPT_TEST_CASE(
    worker_runtime_focus_require_false_still_recovers_from_switch_failure) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.host.focus_require = false;
  h.host.wps_pids = {77};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.focus_require = false;
  target.focus_strategy = "switch";
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.rebuild();

  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  h.Step();  // Monitoring
  h.presentation.show.slide = 2;
  h.Step();  // CoverIn
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.Step();  // EdgeStarting + LaunchEdge
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::EdgeStarting);
  h.launcher.set_alive(false);
  h.windows.windows = {MakeWpsShowWindow(9, 77, 1920, 1080)};
  h.Step();  // EdgeClosed before ready -> recovery + emergency focus
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);

  bool activated = false;
  for (const auto& call : h.windows.activate_with_calls) {
    if (call.first != WindowId{9}) continue;
    activated = true;
    HTMLPPT_CHECK(call.second == FocusStrategy::Auto);
  }
  HTMLPPT_CHECK(activated);
}

// The show-start focus must be re-derived from the host baseline; otherwise a
// previous target's focus.strategy would leak into the restarted show.
HTMLPPT_TEST_CASE(worker_runtime_show_start_focus_uses_baseline_timing) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.host.wps_pids = {77};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.focus_strategy = "switch";
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.rebuild();

  ReachReturning(h, MakeWpsShowWindow(9, 77, 1920, 1080));
  ReturnToMonitoring(h);
  h.windows.activate_calls.clear();
  h.windows.activate_with_calls.clear();
  RestartShow(h);

  bool activated = false;
  for (const auto& call : h.windows.activate_with_calls) {
    if (call.first != WindowId{9}) continue;
    activated = true;
    HTMLPPT_CHECK(call.second == FocusStrategy::Auto);
  }
  HTMLPPT_CHECK(activated);
}

// Same leak check for the screen: the target's 640x360 monitor made this window
// acceptable, but the show-start focus must fall back to the 1920x1080 host
// screen and reject it.
HTMLPPT_TEST_CASE(worker_runtime_show_start_focus_uses_baseline_screen) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.host.wps_pids = {77};
  h.host.screen = ScreenMetrics{1920, 1080};
  htmlppt::TargetPlan target = MakeTarget(1, "t1", "p1");
  target.screen_width = 640;
  target.screen_height = 360;
  h.plan.machine.targets = {target};
  h.plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  h.rebuild();

  ReachReturning(h, MakeWpsShowWindow(9, 77, 600, 330));
  ReturnToMonitoring(h);
  h.windows.activate_calls.clear();
  h.windows.activate_with_calls.clear();
  RestartShow(h);

  for (const auto id : h.windows.activate_calls) {
    HTMLPPT_CHECK(id != WindowId{9});
  }
}

// The safe point is the loop's bounded idle wait, not the microsecond between
// two Steps: a worker parked in Monitoring must advertise it for essentially the
// whole poll interval, which is what lets a 200 ms heartbeat observe it. The real
// SteadyClock is required here; the fake clock returns from sleep_until without
// passing time.
HTMLPPT_TEST_CASE(worker_runtime_marks_the_parked_poll_as_a_safe_point) {
  htmlppt::SteadyClock clock;
  htmlppt::WorkerPlan plan;
  htmlppt::StateMachineConfig config;
  WorkerHostConfig host;
  host.active_poll_interval = Duration{50};
  host.poll_interval = Duration{50};
  htmlppt::test::FakePresentationHost presentation;
  htmlppt::test::FakeOverlay overlay;
  htmlppt::test::FakeLauncher launcher;
  htmlppt::test::FakeWindowSystem windows;
  presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  htmlppt::WorkerRuntime runtime(
      plan, config,
      htmlppt::WorkerPorts{presentation, overlay, launcher, windows, clock},
      host);

  std::atomic<bool> safe_point{false};
  runtime.SetSafePointFlag(&safe_point);
  runtime.Start();
  std::thread loop([&runtime] { runtime.Step(); });

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!safe_point.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // The Step thread is parked in Monitoring: Monitoring heartbeats may apply a
  // reload here.
  HTMLPPT_CHECK(safe_point.load(std::memory_order_relaxed));
  loop.join();
  // The flag covers only the wait; a Step that returned (or is about to run) is
  // not a safe point.
  HTMLPPT_CHECK(!safe_point.load(std::memory_order_relaxed));
  HTMLPPT_CHECK_EQ(runtime.snapshot().state, State::Monitoring);

  runtime.RequestStop("test stop");
  HTMLPPT_CHECK(!runtime.Step());
  HTMLPPT_CHECK(runtime.done());
  HTMLPPT_CHECK(!safe_point.load(std::memory_order_relaxed));
}