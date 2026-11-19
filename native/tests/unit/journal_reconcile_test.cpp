#include "test_main.h"

#include <string>
#include <utility>

#include "core/journal_reconcile.h"

using htmlppt::JournalRecord;
using htmlppt::ReconcileDecision;
using htmlppt::ReconcileFacts;
using htmlppt::ReconcileOutcome;

namespace {

JournalRecord Record(std::string run_id, std::string state, int slide) {
  JournalRecord record;
  record.run_id = std::move(run_id);
  record.state = std::move(state);
  record.slide = slide;
  return record;
}

} // namespace

HTMLPPT_TEST_CASE(reconcile_empty_journal_is_clean) {
  const auto decision =
      htmlppt::ReconcileStartup({}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
}

HTMLPPT_TEST_CASE(reconcile_monitoring_is_clean) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Monitoring", 0)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
}

HTMLPPT_TEST_CASE(reconcile_live_restores_wps_when_possible) {
  ReconcileFacts facts;
  facts.can_restore_wps = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Live", 5)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::RestoreWps);
  HTMLPPT_CHECK(decision.resume_wps);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
}

HTMLPPT_TEST_CASE(reconcile_live_cleans_up_leftover_edge) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Live", 5)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Cleanup);
  HTMLPPT_CHECK(decision.kill_edge);
}

HTMLPPT_TEST_CASE(reconcile_different_run_cleans_up) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  facts.overlay_owned = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-old", "Live", 5)}, facts, "run-new");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Cleanup);
  HTMLPPT_CHECK(decision.kill_edge);
  HTMLPPT_CHECK(decision.hide_overlay);
}

// A run that stopped cleanly journals its terminal Exited state. That state is
// already released, so the next start -- even under a different run id -- must
// not "clean up" it and journal "left state behind"; it is clean with no
// leftover actions to take.
HTMLPPT_TEST_CASE(reconcile_exited_from_a_foreign_run_is_clean) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  facts.overlay_owned = true;
  facts.can_restore_wps = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-done", "Exited", 5)}, facts, "run-new");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
  HTMLPPT_CHECK(!decision.kill_edge);
  HTMLPPT_CHECK(!decision.hide_overlay);
  HTMLPPT_CHECK(!decision.resume_wps);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
  HTMLPPT_CHECK(decision.reason.find("exited cleanly") != std::string::npos);
}

HTMLPPT_TEST_CASE(reconcile_exited_after_stopping_is_clean) {
  // The terminal state wins over a preceding Stopping record: LastStateRecord
  // picks the most recent state, and a clean stop must not become a cleanup.
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Stopping", 5), Record("run-1", "Exited", 5)},
      ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
  HTMLPPT_CHECK(decision.reason.find("exited cleanly") != std::string::npos);
}

HTMLPPT_TEST_CASE(reconcile_faulted_enters_safe_mode) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Faulted", 5)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::SafeMode);
  // Only a real Exited stop is the terminal clean state; a failed run must never
  // be classified as clean just because its supervisor's cleanup ran.
  HTMLPPT_CHECK(decision.outcome != ReconcileOutcome::Clean);
}

// A failed run's Faulted state is leftover state when the next start uses a new
// run id: it must be cleaned up like any other foreign leftover, never treated
// as a completed run.
HTMLPPT_TEST_CASE(reconcile_faulted_from_a_foreign_run_is_cleanup) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-failed", "Faulted", 5)}, facts, "run-next");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Cleanup);
  HTMLPPT_CHECK(decision.outcome != ReconcileOutcome::Clean);
  HTMLPPT_CHECK(decision.kill_edge);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
}

HTMLPPT_TEST_CASE(reconcile_restore_wps_also_cleans_leftovers) {
  ReconcileFacts facts;
  facts.can_restore_wps = true;
  facts.edge_process_alive = true;
  facts.overlay_owned = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Live", 5)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::RestoreWps);
  HTMLPPT_CHECK(decision.resume_wps);
  HTMLPPT_CHECK(decision.kill_edge);
  HTMLPPT_CHECK(decision.hide_overlay);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
}

HTMLPPT_TEST_CASE(reconcile_faulted_run_enters_safe_mode) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  facts.overlay_owned = true;
  facts.can_restore_wps = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Faulted", 5)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::SafeMode);
  HTMLPPT_CHECK(decision.kill_edge);
  HTMLPPT_CHECK(decision.hide_overlay);
  HTMLPPT_CHECK(decision.resume_wps);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
  HTMLPPT_CHECK(decision.reason.find("Faulted") != std::string::npos);
}

// A run that ended in SafeMode takes the same cleanup branch as Faulted; the
// reason names the state so the journal distinguishes the two.
HTMLPPT_TEST_CASE(reconcile_safe_mode_run_enters_safe_mode) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  facts.overlay_owned = true;
  facts.can_restore_wps = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "SafeMode", 3)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::SafeMode);
  HTMLPPT_CHECK(decision.kill_edge);
  HTMLPPT_CHECK(decision.hide_overlay);
  HTMLPPT_CHECK(decision.resume_wps);
  HTMLPPT_CHECK_EQ(decision.target_slide, 3);
  HTMLPPT_CHECK(decision.reason.find("SafeMode") != std::string::npos);
}

HTMLPPT_TEST_CASE(reconcile_clean_preserves_target_slide) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Live", 5)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
  HTMLPPT_CHECK(!decision.kill_edge);
  HTMLPPT_CHECK(!decision.hide_overlay);
  HTMLPPT_CHECK(!decision.resume_wps);
}
