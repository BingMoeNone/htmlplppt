#pragma once

namespace htmlppt {

// What a pending reload request means at one heartbeat. The supervisor may only
// swap the plan at a real safe point: the session marks the instant between
// Steps while it is parked in Monitoring. A heartbeat's state byte alone is not
// enough -- the helper thread may carry the last published Monitoring state
// while the loop is inside a blocking Step -- so the session sends a safe-point
// flag and every other instant defers.
enum class ReloadDecision {
  Apply,  // a request is pending and the worker is at a safe point
  Defer,  // a request is pending but the worker is mid-transition
  Idle,   // no request
};

ReloadDecision DecideReload(bool request_pending, bool at_safe_point);

// True when the deferral must be journalled now. The supervisor journals each
// request file once, not once per heartbeat, so a transition that lasts seconds
// cannot flood the journal; `already_journalled` is the per-request latch the
// supervisor resets when the request file disappears or is applied.
bool ShouldJournalDefer(ReloadDecision decision, bool already_journalled);

} // namespace htmlppt
