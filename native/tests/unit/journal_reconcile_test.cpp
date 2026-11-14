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

HTMLPPT_TEST_CASE(reconcile_faulted_enters_safe_mode) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Faulted", 5)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::SafeMode);
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

HTMLPPT_TEST_CASE(reconcile_safe_mode_cleans_leftovers) {
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
}

HTMLPPT_TEST_CASE(reconcile_clean_preserves_target_slide) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Monitoring", 5)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
  HTMLPPT_CHECK(!decision.kill_edge);
  HTMLPPT_CHECK(!decision.hide_overlay);
  HTMLPPT_CHECK(!decision.resume_wps);
}
