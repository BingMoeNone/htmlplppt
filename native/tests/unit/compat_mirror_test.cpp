#include "test_main.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/compat_flags.h"
#include "core/compat_mirror.h"
#include "core/types.h"

using htmlppt::FlagWrite;
using htmlppt::FlagsForStateTransition;
using htmlppt::State;

namespace {

constexpr State kAllStates[] = {
    State::Boot,
    State::Warming,
    State::Monitoring,
    State::CoverIn,
    State::EdgeStarting,
    State::ShowExit,
    State::Live,
    State::Returning,
    State::ReturnCoverOut,
    State::Stopping,
    State::Faulted,
    State::SafeMode,
    State::Exited,
    State::DegradedNoSupervisor,
};

FlagWrite Row(std::string_view name, std::string payload) {
  return FlagWrite{std::string(name), std::move(payload)};
}

// Compares the rows in order, which is the protocol's write order.
void ExpectRows(const std::vector<FlagWrite>& actual,
                const std::vector<FlagWrite>& expected) {
  HTMLPPT_CHECK_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    HTMLPPT_CHECK_EQ(actual[index].name, expected[index].name);
    HTMLPPT_CHECK_EQ(actual[index].payload, expected[index].payload);
  }
}

} // namespace

HTMLPPT_TEST_CASE(compat_mirror_identical_states_write_nothing) {
  for (const State state : kAllStates) {
    ExpectRows(FlagsForStateTransition(state, state, "profile"), {});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_cover_in_writes_cover_fade) {
  // The transition row is independent of the state it comes from.
  for (const State from : kAllStates) {
    if (from == State::CoverIn) continue;
    ExpectRows(FlagsForStateTransition(from, State::CoverIn, "profile"),
               {Row(htmlppt::kCoverFadeFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_live_writes_cover_ready_then_agent_arm) {
  const std::string payload = "tef-live-page1|exit=0|ready=1";
  for (const State from : kAllStates) {
    if (from == State::Live) continue;
    ExpectRows(
        FlagsForStateTransition(from, State::Live, "tef-live-page1"),
        {Row(htmlppt::kCoverReadyFlag, ""),
         Row(htmlppt::kAgentArmFlag, payload)});
  }
  // The payload keeps an empty profile and still carries both flags.
  ExpectRows(FlagsForStateTransition(State::ShowExit, State::Live, ""),
             {Row(htmlppt::kCoverReadyFlag, ""),
              Row(htmlppt::kAgentArmFlag, "|exit=0|ready=1")});
}

HTMLPPT_TEST_CASE(compat_mirror_entering_safe_mode_writes_cover_hide) {
  for (const State from : kAllStates) {
    if (from == State::SafeMode) continue;
    ExpectRows(FlagsForStateTransition(from, State::SafeMode, "profile"),
               {Row(htmlppt::kCoverHideFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_stopping_writes_cover_stop_then_agent_stop) {
  for (const State from : kAllStates) {
    if (from == State::Stopping) continue;
    ExpectRows(FlagsForStateTransition(from, State::Stopping, "profile"),
               {Row(htmlppt::kCoverStopFlag, ""),
                Row(htmlppt::kAgentStopFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_ending_live_writes_edge_closed) {
  // The runtime closes Edge when it leaves Live for the return path, so every
  // entry into a return-path state must close the session, not just Monitoring
  // (a real return traverses Returning and ReturnCoverOut first).
  ExpectRows(FlagsForStateTransition(State::Live, State::Monitoring, "profile"),
             {Row(htmlppt::kEdgeClosedFlag, "")});
  ExpectRows(FlagsForStateTransition(State::Live, State::Returning, "profile"),
             {Row(htmlppt::kEdgeClosedFlag, "")});
  ExpectRows(FlagsForStateTransition(State::Live, State::ReturnCoverOut,
                                     "profile"),
             {Row(htmlppt::kEdgeClosedFlag, "")});
  // Another state entering a return-path state is not the end of a live
  // session and must not look like a closed Edge.
  for (const State from : kAllStates) {
    if (from == State::Live || from == State::Monitoring) continue;
    ExpectRows(FlagsForStateTransition(from, State::Monitoring, "profile"), {});
  }
  for (const State from : kAllStates) {
    if (from == State::Live || from == State::Returning) continue;
    ExpectRows(FlagsForStateTransition(from, State::Returning, "profile"), {});
  }
  for (const State from : kAllStates) {
    if (from == State::Live || from == State::ReturnCoverOut) continue;
    ExpectRows(FlagsForStateTransition(from, State::ReturnCoverOut, "profile"),
               {});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_exited_writes_the_stop_flags) {
  // A real stop often never publishes Stopping: the runtime feeds CleanupDone in
  // the same Step, so the observable transition is ... -> Exited (from Live,
  // Monitoring, or a directly observed Stopping).
  for (const State from : kAllStates) {
    if (from == State::Exited) continue;
    ExpectRows(FlagsForStateTransition(from, State::Exited, "profile"),
               {Row(htmlppt::kCoverStopFlag, ""),
                Row(htmlppt::kAgentStopFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_full_live_cycle_emits_posted_flags_in_order) {
  constexpr State kCycle[] = {State::CoverIn, State::Live, State::Returning,
                              State::ReturnCoverOut, State::Monitoring};
  std::vector<FlagWrite> writes;
  State from = State::Boot;
  for (const State to : kCycle) {
    for (const FlagWrite& row :
         FlagsForStateTransition(from, to, "tef-live-page1")) {
      writes.push_back(row);
    }
    from = to;
  }
  ExpectRows(writes,
             {Row(htmlppt::kCoverFadeFlag, ""),
              Row(htmlppt::kCoverReadyFlag, ""),
              Row(htmlppt::kAgentArmFlag, "tef-live-page1|exit=0|ready=1"),
              Row(htmlppt::kEdgeClosedFlag, "")});
}

HTMLPPT_TEST_CASE(compat_mirror_unrelated_transitions_write_nothing) {
  constexpr struct {
    State from;
    State to;
  } kUnrelated[] = {
      {State::Boot, State::Warming},
      {State::Warming, State::Monitoring},
      {State::Monitoring, State::EdgeStarting},
      {State::Returning, State::ReturnCoverOut},
      {State::ReturnCoverOut, State::Monitoring},
      {State::Faulted, State::Monitoring},
      {State::Monitoring, State::Faulted},
      {State::Exited, State::Boot},
      {State::DegradedNoSupervisor, State::Warming},
  };
  for (const auto& row : kUnrelated) {
    ExpectRows(FlagsForStateTransition(row.from, row.to, "profile"), {});
  }
}
