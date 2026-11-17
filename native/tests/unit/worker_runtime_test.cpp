#include "test_main.h"

#include <string>
#include <string_view>
#include <vector>

#include "core/presentation_host.h"
#include "core/types.h"
#include "worker_fakes.h"

using htmlppt::HostStatus;
using htmlppt::State;
using htmlppt::test::MakeShowState;
using htmlppt::test::WorkerHarness;

namespace {

bool JournalContains(const std::vector<std::string>& journal,
                     std::string_view text) {
  for (const auto& entry : journal) {
    if (entry.find(text) != std::string::npos) return true;
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(worker_runtime_starts_show_and_tracks_slide_changes) {
  WorkerHarness h;                       // fakes from worker_fakes.h
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Warming);   // Start only
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  h.presentation.show.slide = 2;
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().prev_slide, 2);
}

// A busy COM poll reports a transient COM failure and keeps the last good show
// sample, so it can never look like a slide change or a show end.
HTMLPPT_TEST_CASE(worker_runtime_reports_com_busy_without_show_ended) {
  WorkerHarness h;
  h.presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  h.Start(); h.runtime.Step();
  h.presentation.show.status = HostStatus::Busy;
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  HTMLPPT_CHECK(h.presentation.exit_show_calls == 0);
  HTMLPPT_CHECK(JournalContains(h.runtime.journal(), "COM busy"));
}
