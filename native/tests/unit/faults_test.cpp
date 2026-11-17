#include "test_main.h"

#include <string>

#include "core/faults.h"

using htmlppt::Faults;
using htmlppt::FaultsToString;
using htmlppt::HasAnyFault;
using htmlppt::ParseFaults;

HTMLPPT_TEST_CASE(faults_parse_every_switch) {
  const Faults faults =
      ParseFaults("com-hang,com-462, edge-slow cover-hang;focus-deny,supervisor-loss,sleep-resume multi-monitor");
  HTMLPPT_CHECK(faults.com_hang);
  HTMLPPT_CHECK(faults.com_462);
  HTMLPPT_CHECK(faults.edge_slow);
  HTMLPPT_CHECK(faults.cover_hang);
  HTMLPPT_CHECK(faults.focus_deny);
  HTMLPPT_CHECK(faults.supervisor_loss);
  HTMLPPT_CHECK(faults.sleep_resume);
  HTMLPPT_CHECK(faults.multi_monitor);
}

HTMLPPT_TEST_CASE(faults_default_to_off_and_ignore_typos) {
  const Faults empty = ParseFaults("");
  HTMLPPT_CHECK(!HasAnyFault(empty));
  HTMLPPT_CHECK_EQ(FaultsToString(empty), std::string{});

  const Faults typo = ParseFaults("com-hang,com-hnag,edge_slow");
  HTMLPPT_CHECK(typo.com_hang);
  HTMLPPT_CHECK(!typo.edge_slow);  // underscore is not a token separator
  HTMLPPT_CHECK_EQ(FaultsToString(typo), std::string{"com-hang"});
}

HTMLPPT_TEST_CASE(faults_round_trip_through_the_text_form) {
  const Faults faults = ParseFaults("cover-hang,focus-deny");
  const Faults parsed = ParseFaults(FaultsToString(faults));
  HTMLPPT_CHECK_EQ(FaultsToString(parsed), FaultsToString(faults));
  HTMLPPT_CHECK(parsed.cover_hang);
  HTMLPPT_CHECK(parsed.focus_deny);
}
