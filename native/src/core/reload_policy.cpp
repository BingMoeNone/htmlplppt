#include "core/reload_policy.h"

namespace htmlppt {

ReloadDecision DecideReload(bool request_pending, bool at_safe_point) {
  if (!request_pending) return ReloadDecision::Idle;
  return at_safe_point ? ReloadDecision::Apply : ReloadDecision::Defer;
}

bool ShouldJournalDefer(ReloadDecision decision, bool already_journalled) {
  return decision == ReloadDecision::Defer && !already_journalled;
}

} // namespace htmlppt
