#include "test_main.h"

#include <filesystem>
#include <fstream>

#include "core/reload_policy.h"
#include "test_utils.h"

using htmlppt::ClearFailedReloadRequest;
using htmlppt::ClearStaleReloadRequest;
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

// A failed rebuild must not leave the request pending: without the latch every
// heartbeat would pause the worker, rebuild the same broken plan, and journal
// again. A request that cannot even be deleted latches; every other outcome
// clears it.
HTMLPPT_TEST_CASE(reload_policy_latches_a_failed_rebuild_only_when_stuck) {
  htmlppt::test::TempDir dir;
  const auto missing = ClearFailedReloadRequest(dir.path());
  HTMLPPT_CHECK(missing.request_cleared);
  HTMLPPT_CHECK(!missing.latch);

  {
    std::ofstream request(dir.path() / "reload.request", std::ios::binary);
    HTMLPPT_CHECK(request.is_open());
  }
  const auto cleared = ClearFailedReloadRequest(dir.path());
  HTMLPPT_CHECK(cleared.request_cleared);
  HTMLPPT_CHECK(!cleared.latch);
  HTMLPPT_CHECK(!std::filesystem::exists(dir.path() / "reload.request"));

  std::filesystem::create_directories(dir.path() / "reload.request" / "child");
  const auto stuck = ClearFailedReloadRequest(dir.path());
  HTMLPPT_CHECK(!stuck.request_cleared);
  HTMLPPT_CHECK(stuck.latch);
}


// supervisor clears any marker left in the state directory before it starts a
// new run, so an old request cannot restart the fresh worker at its first safe
// point. A missing marker is success too -- the common case.
HTMLPPT_TEST_CASE(reload_policy_clears_a_stale_request) {
  htmlppt::test::TempDir dir;
  HTMLPPT_CHECK(ClearStaleReloadRequest(dir.path()));
  HTMLPPT_CHECK(!std::filesystem::exists(dir.path() / "reload.request"));
  {
    std::ofstream request(dir.path() / "reload.request", std::ios::binary);
    HTMLPPT_CHECK(request.is_open());
  }
  HTMLPPT_CHECK(ClearStaleReloadRequest(dir.path()));
  HTMLPPT_CHECK(!std::filesystem::exists(dir.path() / "reload.request"));
}

// The one case the supervisor must report: the marker exists and cannot be
// removed. Reporting it as success would let the next safe point replay the
// stale request after all.
HTMLPPT_TEST_CASE(reload_policy_reports_a_request_it_cannot_remove) {
  htmlppt::test::TempDir dir;
  std::filesystem::create_directories(dir.path() / "reload.request" / "child");
  HTMLPPT_CHECK(!ClearStaleReloadRequest(dir.path()));
  HTMLPPT_CHECK(std::filesystem::exists(dir.path() / "reload.request"));
}
