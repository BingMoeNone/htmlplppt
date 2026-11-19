#include "test_main.h"

#include "core/reload_policy.h"

using htmlppt::DecideReload;
using htmlppt::ReloadDecision;
using htmlppt::ShouldJournalDefer;

// The supervisor may swap the plan only at a real safe point: the session marks
// the instant between Steps while the worker is parked in Monitoring. Every
// other instant is inside a startup, switch/return, or stop step, where a
// restart would drop that transition.
HTMLPPT_TEST_CASE(reload_policy_applies_only_at_a_safe_point) {
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/true,
                             /*at_safe_point=*/true) ==
                ReloadDecision::Apply);
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/true,
                             /*at_safe_point=*/false) ==
                ReloadDecision::Defer);
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/false,
                             /*at_safe_point=*/true) ==
                ReloadDecision::Idle);
  // No request is Idle even when the worker is not at a safe point: "Defer" is
  // reserved for a request that is waiting for one.
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/false,
                             /*at_safe_point=*/false) ==
                ReloadDecision::Idle);
}

// The heartbeat's state byte can read Monitoring while the loop is actually
// inside a blocking Step: the helper thread may carry the last published state.
// Only the safe-point flag proves the loop is between Steps, so a stale
// monitoring heartbeat must defer, not apply.
HTMLPPT_TEST_CASE(reload_policy_defers_a_stale_monitoring_heartbeat) {
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/true,
                             /*at_safe_point=*/false) ==
                ReloadDecision::Defer);
}

// A deferred request must not journal one line per heartbeat while the
// transition lasts; the latch makes it one line per request file.
HTMLPPT_TEST_CASE(reload_policy_journals_a_deferral_once) {
  HTMLPPT_CHECK(ShouldJournalDefer(ReloadDecision::Defer,
                                   /*already_journalled=*/false));
  HTMLPPT_CHECK(!ShouldJournalDefer(ReloadDecision::Defer,
                                    /*already_journalled=*/true));
  // Only a deferral is journalled here; Apply and Idle have their own events.
  HTMLPPT_CHECK(!ShouldJournalDefer(ReloadDecision::Apply,
                                    /*already_journalled=*/false));
  HTMLPPT_CHECK(!ShouldJournalDefer(ReloadDecision::Idle,
                                    /*already_journalled=*/false));
}
