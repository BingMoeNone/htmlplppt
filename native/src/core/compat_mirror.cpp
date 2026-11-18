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
    case State::Monitoring:
      // Only the end of a live session closes Edge; a return path that reaches
      // Monitoring through the cover states must not look like one.
      if (from == State::Live) return {EmptyFlag(kEdgeClosedFlag)};
      return {};
    case State::Boot:
    case State::Warming:
    case State::EdgeStarting:
    case State::ShowExit:
    case State::Returning:
    case State::ReturnCoverOut:
    case State::Faulted:
    case State::Exited:
    case State::DegradedNoSupervisor:
      return {};
  }
  return {};
}

} // namespace htmlppt
