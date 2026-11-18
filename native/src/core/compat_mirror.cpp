#include "core/compat_mirror.h"

#include "core/compat_flags.h"

namespace htmlppt {
namespace {

FlagWrite EmptyFlag(std::string_view name) {
  return FlagWrite{std::string(name), std::string{}};
}

} // namespace

std::vector<FlagWrite> FlagsForStateTransition(State from, State to,
                                               std::string_view edge_profile) {
  // A step that publishes the same state is not a transition: a steady Live loop
  // must not rewrite cover-ready/agent-arm on every poll.
  if (from == to) return {};
  switch (to) {
    case State::CoverIn:
      return {EmptyFlag(kCoverFadeFlag)};
    case State::Live:
      return {EmptyFlag(kCoverReadyFlag),
              FlagWrite{std::string(kAgentArmFlag),
                        std::string(edge_profile) + "|exit=0|ready=1"}};
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
