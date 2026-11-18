#include "test_main.h"

#include "core/reload_policy.h"

using htmlppt::DecideReload;
using htmlppt::ReloadDecision;
using htmlppt::ShouldJournalDefer;

// The supervisor may swap the plan only between transitions: the worker is
// parked in Monitoring then, and every other state is mid-switch/return (or
// startup/stop), where a restart would drop the transition.
HTMLPPT_TEST_CASE(reload_policy_applies_only_while_monitoring) {
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/true,
                             /*worker_state_is_monitoring=*/true) ==
                ReloadDecision::Apply);
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/true,
                             /*worker_state_is_monitoring=*/false) ==
                ReloadDecision::Defer);
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/false,
                             /*worker_state_is_monitoring=*/true) ==
                ReloadDecision::Idle);
  // No request is Idle even when the worker is busy: "Defer" is reserved for a
  // request that is waiting for a safe point.
  HTMLPPT_CHECK(DecideReload(/*request_pending=*/false,
                             /*worker_state_is_monitoring=*/false) ==
                ReloadDecision::Idle);
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
