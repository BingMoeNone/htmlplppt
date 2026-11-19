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

constexpr std::string_view kFlagDir = R"(C:\compat)";

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

// focus-agent.ps1 parses agent-arm.flag as "<profile>|<exitFlag>|<readyFlag>"
// and writes those two paths itself, so the payload is split on '|' exactly the
// way the script splits it.
std::vector<std::string> SplitAgentArm(std::string_view payload) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t bar = payload.find('|', start);
    if (bar == std::string_view::npos) {
      parts.emplace_back(payload.substr(start));
      return parts;
    }
    parts.emplace_back(payload.substr(start, bar - start));
    start = bar + 1;
  }
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

// Asserts the entering-Live row pair and the legacy consumer's view of the
// agent-arm payload: three fields, and fields 2 and 3 are the two flag paths the
// focus agent must write.
void ExpectAgentArm(const std::vector<FlagWrite>& rows, std::string profile,
                    std::string_view flag_dir) {
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(rows[0].name, std::string(htmlppt::kCoverReadyFlag));
  HTMLPPT_CHECK_EQ(rows[1].name, std::string(htmlppt::kAgentArmFlag));
  const std::vector<std::string> parts = SplitAgentArm(rows[1].payload);
  HTMLPPT_CHECK_EQ(parts.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(parts[0], profile);
  HTMLPPT_CHECK(EndsWith(parts[1], "edge-closed.flag"));
  HTMLPPT_CHECK(EndsWith(parts[2], "cover-ready.flag"));
  // The agent runs with its own working directory, so the base directory must be
  // spelled out with Windows separators rather than left relative.
  HTMLPPT_CHECK_EQ(parts[1], std::string(flag_dir) + "\\edge-closed.flag");
  HTMLPPT_CHECK_EQ(parts[2], std::string(flag_dir) + "\\cover-ready.flag");
}

} // namespace

HTMLPPT_TEST_CASE(compat_mirror_identical_states_write_nothing) {
  for (const State state : kAllStates) {
    ExpectRows(FlagsForStateTransition(state, state, "profile", kFlagDir), {});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_cover_in_writes_cover_fade) {
  // The transition row is independent of the state it comes from.
  for (const State from : kAllStates) {
    if (from == State::CoverIn) continue;
    ExpectRows(FlagsForStateTransition(from, State::CoverIn, "profile",
                                       kFlagDir),
               {Row(htmlppt::kCoverFadeFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_live_writes_cover_ready_then_agent_arm) {
  for (const State from : kAllStates) {
    if (from == State::Live) continue;
    ExpectAgentArm(
        FlagsForStateTransition(from, State::Live, "tef-live-page1", kFlagDir),
        "tef-live-page1", kFlagDir);
  }
  // The payload keeps an empty profile and still carries both flag paths.
  ExpectAgentArm(
      FlagsForStateTransition(State::ShowExit, State::Live, "", kFlagDir), "",
      kFlagDir);
}

HTMLPPT_TEST_CASE(compat_mirror_entering_safe_mode_writes_cover_hide) {
  for (const State from : kAllStates) {
    if (from == State::SafeMode) continue;
    ExpectRows(FlagsForStateTransition(from, State::SafeMode, "profile",
                                       kFlagDir),
               {Row(htmlppt::kCoverHideFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_stopping_writes_cover_stop_then_agent_stop) {
  for (const State from : kAllStates) {
    if (from == State::Stopping) continue;
    ExpectRows(FlagsForStateTransition(from, State::Stopping, "profile",
                                       kFlagDir),
               {Row(htmlppt::kCoverStopFlag, ""),
                Row(htmlppt::kAgentStopFlag, "")});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_ending_live_writes_edge_closed) {
  // The runtime closes Edge when it leaves Live for the return path, so every
  // entry into a return-path state must close the session, not just Monitoring
  // (a real return traverses Returning and ReturnCoverOut first).
  ExpectRows(
      FlagsForStateTransition(State::Live, State::Monitoring, "profile",
                              kFlagDir),
      {Row(htmlppt::kEdgeClosedFlag, "")});
  ExpectRows(
      FlagsForStateTransition(State::Live, State::Returning, "profile",
                              kFlagDir),
      {Row(htmlppt::kEdgeClosedFlag, "")});
  ExpectRows(
      FlagsForStateTransition(State::Live, State::ReturnCoverOut, "profile",
                              kFlagDir),
      {Row(htmlppt::kEdgeClosedFlag, "")});
  // Another state entering a return-path state is not the end of a live
  // session and must not look like a closed Edge.
  for (const State from : kAllStates) {
    if (from == State::Live || from == State::Monitoring) continue;
    ExpectRows(
        FlagsForStateTransition(from, State::Monitoring, "profile", kFlagDir),
        {});
  }
  for (const State from : kAllStates) {
    if (from == State::Live || from == State::Returning) continue;
    ExpectRows(
        FlagsForStateTransition(from, State::Returning, "profile", kFlagDir),
        {});
  }
  for (const State from : kAllStates) {
    if (from == State::Live || from == State::ReturnCoverOut) continue;
    ExpectRows(FlagsForStateTransition(from, State::ReturnCoverOut, "profile",
                                       kFlagDir),
               {});
  }
}

HTMLPPT_TEST_CASE(compat_mirror_entering_exited_writes_the_stop_flags) {
  // A real stop often never publishes Stopping: the runtime feeds CleanupDone in
  // the same Step, so the observable transition is ... -> Exited (from Live,
  // Monitoring, or a directly observed Stopping).
  for (const State from : kAllStates) {
    if (from == State::Exited) continue;
    ExpectRows(FlagsForStateTransition(from, State::Exited, "profile",
                                       kFlagDir),
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
         FlagsForStateTransition(from, to, "tef-live-page1", kFlagDir)) {
      writes.push_back(row);
    }
    from = to;
  }
  ExpectRows(writes,
             {Row(htmlppt::kCoverFadeFlag, ""),
              Row(htmlppt::kCoverReadyFlag, ""),
              Row(htmlppt::kAgentArmFlag,
                  "tef-live-page1|C:\\compat\\edge-closed.flag|"
                  "C:\\compat\\cover-ready.flag"),
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
    ExpectRows(FlagsForStateTransition(row.from, row.to, "profile", kFlagDir),
               {});
  }
}
