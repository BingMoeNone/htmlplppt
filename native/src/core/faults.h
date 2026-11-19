#pragma once

#include <string>
#include <string_view>

namespace htmlppt {

// Fault-injection switches (design spec 6.6). All off in production; they let a
// real machine exercise the resilience paths without touching WPS or the OS.
// Enabled through the worker's environment: HTMLPPT_FAULTS=com-hang,edge-slow
struct Faults {
  bool com_hang = false;   // COM queries never answer: drives the error window
  bool com_462 = false;    // COM busy with the classic WPS transient
  bool edge_slow = false;  // Edge reports ready late (cold start)
  bool cover_hang = false; // the cover never reaches its hold
  bool focus_deny = false; // activation always fails (no Edge window, no focus)
  bool supervisor_loss = false;  // the worker is told the supervisor is gone
  bool sleep_resume = false;     // inject a long suspend gap + resume
  bool multi_monitor = false;    // inject a display-topology change
  bool cleanup_slow = false;     // cleanup starts after a 3 s sleep (stop budget)
};

// Parses a comma- or space-separated list: com-hang, com-462, edge-slow, supervisor-loss,
// cover-hang, focus-deny, sleep-resume, multi-monitor, cleanup-slow. Unknown
// names are ignored so a typo cannot fail a demo run.
Faults ParseFaults(std::string_view text);

// Human-readable list of the active faults, empty when none are set. Used for
// the journal line that makes an injected run obvious in the logs.
std::string FaultsToString(const Faults& faults);

bool HasAnyFault(const Faults& faults);

} // namespace htmlppt
