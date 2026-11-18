#include "core/reload_policy.h"

namespace htmlppt {

ReloadDecision DecideReload(bool request_pending,
                            bool worker_state_is_monitoring) {
  if (!request_pending) return ReloadDecision::Idle;
  return worker_state_is_monitoring ? ReloadDecision::Apply
                                    : ReloadDecision::Defer;
}

bool ShouldJournalDefer(ReloadDecision decision, bool already_journalled) {
  return decision == ReloadDecision::Defer && !already_journalled;
}

} // namespace htmlppt
