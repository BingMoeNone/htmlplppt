#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/journal.h"

namespace htmlppt {

enum class ReconcileOutcome {
  Clean,
  TakeOver,
  Cleanup,
  RestoreWps,
  SafeMode,
};

struct ReconcileFacts {
  bool edge_process_alive = false;
  bool overlay_owned = false;
  bool can_restore_wps = false;
  bool flags_owned_by_current_run = false;
};

struct ReconcileDecision {
  ReconcileOutcome outcome = ReconcileOutcome::Clean;
  bool kill_edge = false;
  bool hide_overlay = false;
  bool resume_wps = false;
  int target_slide = 0;
  std::string reason;
};

ReconcileDecision ReconcileStartup(const std::vector<JournalRecord>& tail,
                                   const ReconcileFacts& facts,
                                   std::string_view run_id);

} // namespace htmlppt
