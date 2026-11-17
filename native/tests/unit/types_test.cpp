#include "test_main.h"

#include <set>
#include <string>

#include "core/types.h"

using htmlppt::State;
using htmlppt::StateName;

HTMLPPT_TEST_CASE(state_names_are_stable_and_distinct) {
  // --status prints these to the operator, so the spelling is part of the
  // output contract (the smoke script also matches them).
  HTMLPPT_CHECK_EQ(StateName(State::Boot), std::string{"Boot"});
  HTMLPPT_CHECK_EQ(StateName(State::Monitoring), std::string{"Monitoring"});
  HTMLPPT_CHECK_EQ(StateName(State::ReturnCoverOut), std::string{"ReturnCoverOut"});
  HTMLPPT_CHECK_EQ(StateName(State::DegradedNoSupervisor),
                  std::string{"DegradedNoSupervisor"});

  // Every enumerator must map to a distinct, non-empty name.
  const State all[] = {
      State::Boot,       State::Warming,      State::Monitoring,
      State::CoverIn,    State::EdgeStarting, State::ShowExit,
      State::Live,       State::Returning,    State::ReturnCoverOut,
      State::Stopping,   State::Faulted,      State::SafeMode,
      State::Exited,     State::DegradedNoSupervisor,
  };
  std::set<std::string> names;
  for (const State state : all) {
    const std::string name = StateName(state);
    HTMLPPT_CHECK(!name.empty());
    names.insert(name);
  }
  HTMLPPT_CHECK_EQ(names.size(), std::size_t{14});
}