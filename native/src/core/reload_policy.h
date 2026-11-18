#pragma once

namespace htmlppt {

// What a pending reload request means at one heartbeat. The supervisor may only
// swap the plan while the worker is parked in Monitoring: every other state is
// in the middle of a startup, switch/return, or stop, where replacing the
// worker would drop that transition and desynchronise the deck.
enum class ReloadDecision {
  Apply,  // a request is pending and the worker is Monitoring
  Defer,  // a request is pending but the worker is mid-transition
  Idle,   // no request
};

ReloadDecision DecideReload(bool request_pending,
                            bool worker_state_is_monitoring);

// True when the deferral must be journalled now. The supervisor journals each
// request file once, not once per heartbeat, so a transition that lasts seconds
// cannot flood the journal; `already_journalled` is the per-request latch the
// supervisor resets when the request file disappears or is applied.
bool ShouldJournalDefer(ReloadDecision decision, bool already_journalled);

} // namespace htmlppt
