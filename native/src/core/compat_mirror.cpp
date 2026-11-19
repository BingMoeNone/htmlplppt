#include "core/compat_mirror.h"

#include "core/compat_flags.h"

namespace htmlppt {
namespace {

FlagWrite EmptyFlag(std::string_view name) {
  return FlagWrite{std::string(name), std::string{}};
}

// focus-agent.ps1 expects "<profile>|<exitFlag>|<readyFlag>" and writes the two
// named files itself, so the payload carries the paths under the compat base
// directory. Directories contain no "|", and the legacy parser splits on every
// "|", so the field order is the whole contract.
std::string AgentArmPayload(std::string_view edge_profile,
                            std::string_view flag_dir) {
  std::string payload(edge_profile);
  payload += '|';
  payload += flag_dir;
  payload += '\\';
  payload += kEdgeClosedFlag;
  payload += '|';
  payload += flag_dir;
  payload += '\\';
  payload += kCoverReadyFlag;
  return payload;
}

} // namespace

std::vector<FlagWrite> FlagsForStateTransition(State from, State to,
                                               std::string_view edge_profile,
                                               std::string_view flag_dir) {
  // A step that publishes the same state is not a transition: a steady Live loop
  // must not rewrite cover-ready/agent-arm on every poll.
  if (from == to) return {};
  switch (to) {
    case State::CoverIn:
      return {EmptyFlag(kCoverFadeFlag)};
    case State::Live:
      return {EmptyFlag(kCoverReadyFlag),
              FlagWrite{std::string(kAgentArmFlag),
                        AgentArmPayload(edge_profile, flag_dir)}};
    case State::SafeMode:
      return {EmptyFlag(kCoverHideFlag)};
    case State::Stopping:
      return {EmptyFlag(kCoverStopFlag), EmptyFlag(kAgentStopFlag)};
    case State::Exited:
      // A real stop usually never publishes Stopping: the runtime feeds
      // CleanupDone in the same Step, so the loop observes Stopping -> Exited or
      // even Live/Monitoring -> Exited. The legacy stop tooling watches the stop
      // files, so the observable end must write them too.
      return {EmptyFlag(kCoverStopFlag), EmptyFlag(kAgentStopFlag)};
    case State::Returning:
    case State::ReturnCoverOut:
      // Leaving Live for the return path is when the runtime closes Edge. A real
      // return goes Live -> Returning -> ReturnCoverOut -> Monitoring, so the
      // direct Live -> Monitoring row below is not enough on its own.
      if (from == State::Live) return {EmptyFlag(kEdgeClosedFlag)};
      return {};
    case State::Monitoring:
      // Kept for a driver that publishes the direct Live -> Monitoring edge.
      if (from == State::Live) return {EmptyFlag(kEdgeClosedFlag)};
      return {};
    case State::Boot:
    case State::Warming:
    case State::EdgeStarting:
    case State::ShowExit:
    case State::Faulted:
    case State::DegradedNoSupervisor:
      return {};
  }
  return {};
}

} // namespace htmlppt
