#include "core/journal_reconcile.h"

#include <algorithm>
#include <utility>

namespace htmlppt {
namespace {

const JournalRecord* LastStateRecord(const std::vector<JournalRecord>& tail) {
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
    if (!it->state.empty()) return &*it;
  }
  return nullptr;
}

ReconcileDecision Clean(std::string reason) {
  ReconcileDecision decision;
  decision.outcome = ReconcileOutcome::Clean;
  decision.reason = std::move(reason);
  return decision;
}

} // namespace

ReconcileDecision ReconcileStartup(const std::vector<JournalRecord>& tail,
                                   const ReconcileFacts& facts,
                                   std::string_view run_id) {
  const JournalRecord* last = LastStateRecord(tail);
  if (last == nullptr) return Clean("empty journal");

  ReconcileDecision decision;
  decision.target_slide = last->slide;
  const bool same_run = std::string_view(last->run_id) == run_id;

  if (!same_run) {
    if (facts.flags_owned_by_current_run) {
      decision.outcome = ReconcileOutcome::TakeOver;
      decision.kill_edge = facts.edge_process_alive;
      decision.hide_overlay = facts.overlay_owned;
      decision.resume_wps = facts.can_restore_wps;
      decision.reason = "take over flags from a previous run";
      return decision;
    }
    decision.outcome = ReconcileOutcome::Cleanup;
    decision.kill_edge = facts.edge_process_alive;
    decision.hide_overlay = facts.overlay_owned;
    decision.resume_wps = facts.can_restore_wps;
    decision.reason = "previous run left state behind";
    return decision;
  }

  if (last->state == "Live" || last->state == "Returning") {
    if (facts.can_restore_wps) {
      decision.outcome = ReconcileOutcome::RestoreWps;
      decision.resume_wps = true;
      decision.reason = "restore WPS after an interrupted live/return state";
      return decision;
    }
    if (facts.edge_process_alive || facts.overlay_owned) {
      decision.outcome = ReconcileOutcome::Cleanup;
      decision.kill_edge = facts.edge_process_alive;
      decision.hide_overlay = facts.overlay_owned;
      decision.reason = "clean up an interrupted live/return state";
      return decision;
    }
    return Clean("live/return state has no leftover resources");
  }

  if (last->state == "CoverIn" || last->state == "EdgeStarting" ||
      last->state == "ShowExit") {
    decision.outcome = ReconcileOutcome::Cleanup;
    decision.kill_edge = facts.edge_process_alive;
    decision.hide_overlay = facts.overlay_owned;
    decision.resume_wps = facts.can_restore_wps;
    decision.reason = "clean up an interrupted switch";
    return decision;
  }

  if (last->state == "Faulted" || last->state == "SafeMode") {
    decision.outcome = ReconcileOutcome::SafeMode;
    decision.reason = "previous run ended in " + last->state;
    return decision;
  }

  decision.kill_edge = facts.edge_process_alive;
  decision.hide_overlay = facts.overlay_owned;
  decision.reason = "previous state is safe to resume";
  return decision;
}

} // namespace htmlppt
