#include "test_main.h"

#include "core/failure_policy.h"

using htmlppt::ComErrorWindow;
using htmlppt::Duration;
using htmlppt::FailureKind;
using htmlppt::FakeClock;
using htmlppt::State;

HTMLPPT_TEST_CASE(escalate_failure_maps_switch_states) {
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::CoverIn) == FailureKind::Switch);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::EdgeStarting) ==
                FailureKind::Switch);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::ShowExit) ==
                FailureKind::Switch);
}

HTMLPPT_TEST_CASE(escalate_failure_maps_return_states) {
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Live) == FailureKind::Return);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Returning) ==
                FailureKind::Return);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::ReturnCoverOut) ==
                FailureKind::Return);
}

HTMLPPT_TEST_CASE(escalate_failure_maps_structural_states) {
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Boot) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Warming) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Monitoring) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Faulted) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::SafeMode) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Exited) ==
                FailureKind::Structural);
}

HTMLPPT_TEST_CASE(com_error_window_expires_at_boundary) {
  FakeClock clock;
  ComErrorWindow window(Duration{2000});
  HTMLPPT_CHECK(!window.active());
  window.record(clock.now());
  HTMLPPT_CHECK(window.active());
  clock.advance(Duration{1999});
  HTMLPPT_CHECK(!window.expired(clock.now()));
  clock.advance(Duration{1});
  HTMLPPT_CHECK(window.expired(clock.now()));
}

HTMLPPT_TEST_CASE(com_error_window_reset_clears_state) {
  FakeClock clock;
  ComErrorWindow window(Duration{2000});
  window.record(clock.now());
  window.reset();
  HTMLPPT_CHECK(!window.active());
  HTMLPPT_CHECK(!window.expired(clock.now()));
}

HTMLPPT_TEST_CASE(com_error_window_keeps_first_error_time) {
  FakeClock clock;
  ComErrorWindow window(Duration{2000});
  window.record(clock.now());
  clock.advance(Duration{1500});
  window.record(clock.now());
  clock.advance(Duration{499});
  HTMLPPT_CHECK(!window.expired(clock.now()));
  clock.advance(Duration{1});
  HTMLPPT_CHECK(window.expired(clock.now()));
}
