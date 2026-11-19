#include "core/reload_policy.h"

namespace htmlppt {

ReloadDecision DecideReload(bool request_pending, bool at_safe_point) {
  if (!request_pending) return ReloadDecision::Idle;
  return at_safe_point ? ReloadDecision::Apply : ReloadDecision::Defer;
}

bool ShouldJournalDefer(ReloadDecision decision, bool already_journalled) {
  return decision == ReloadDecision::Defer && !already_journalled;
}

bool ClearStaleReloadRequest(const std::filesystem::path& state_dir) {
  std::error_code error;
  std::filesystem::remove(state_dir / "reload.request", error);
  return !error;
}

FailedReload ClearFailedReloadRequest(const std::filesystem::path& state_dir) {
  FailedReload result;
  result.request_cleared = ClearStaleReloadRequest(state_dir);
  result.latch = !result.request_cleared;
  return result;
}

} // namespace htmlppt
