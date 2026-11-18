#include "test_main.h"

#ifdef _WIN32
#include <cstddef>
#include <string>
#include <vector>

#include "app/supervisor.h"
#include "core/config.h"
#include "core/resolve.h"
#include "core/state_machine_types.h"

using htmlppt::Config;
using htmlppt::DisplayConfig;
using htmlppt::ResolvedTarget;
using htmlppt::ResolveTargetScreens;
using htmlppt::RunPlan;
using htmlppt::TargetConfig;
using htmlppt::TargetPlan;

namespace {

TargetPlan MakePlanTarget(int slide) {
  TargetPlan target;
  target.slide = slide;
  return target;
}

ResolvedTarget MakeSpec(int ordinal, int slide) {
  ResolvedTarget spec;
  spec.ordinal = ordinal;
  spec.slide = slide;
  return spec;
}

} // namespace

// The machine plan cannot carry display metrics from the config parser: screen
// enumeration is a Windows fact. The supervisor fills each plan target from its
// source target's display.monitor, and must leave targets without a display
// block empty so the worker keeps the host screen.
HTMLPPT_TEST_CASE(win32_resolve_target_screens_fills_only_display_targets) {
  Config config;
  TargetConfig with_display;
  with_display.slide = 1;
  DisplayConfig display;
  display.monitor = "primary";
  with_display.display = display;
  TargetConfig without_display;
  without_display.slide = 2;
  config.active.targets = {with_display, without_display};

  RunPlan plan;
  plan.targets = {MakePlanTarget(1), MakePlanTarget(2)};
  const std::vector<ResolvedTarget> resolved = {MakeSpec(1, 1), MakeSpec(2, 2)};

  ResolveTargetScreens(plan, config, resolved);
  HTMLPPT_CHECK(plan.targets[0].screen_width.has_value());
  HTMLPPT_CHECK(plan.targets[0].screen_height.has_value());
  HTMLPPT_CHECK(*plan.targets[0].screen_width > 0);
  HTMLPPT_CHECK(*plan.targets[0].screen_height > 0);
  HTMLPPT_CHECK(!plan.targets[1].screen_width.has_value());
  HTMLPPT_CHECK(!plan.targets[1].screen_height.has_value());
}

// The helper is called with the plan's own index-aligned host-target vector; an
// empty plan must be a no-op rather than an out-of-range read.
HTMLPPT_TEST_CASE(win32_resolve_target_screens_tolerates_an_empty_plan) {
  Config config;
  RunPlan plan;
  ResolveTargetScreens(plan, config, {});
  HTMLPPT_CHECK(plan.targets.empty());
}

// An out-of-range display selector falls back to the primary, exactly like the
// global display path, but the failure must be reportable so it can be warned
// about and journaled instead of silently scoring against the wrong monitor.
HTMLPPT_TEST_CASE(win32_resolve_target_screens_reports_a_selection_error) {
  Config config;
  TargetConfig target;
  target.slide = 1;
  DisplayConfig display;
  display.monitor = "99";
  target.display = display;
  config.active.targets = {target};

  RunPlan plan;
  plan.targets = {MakePlanTarget(1)};
  const std::vector<ResolvedTarget> resolved = {MakeSpec(1, 1)};

  std::vector<std::string> warnings;
  ResolveTargetScreens(plan, config, resolved, &warnings);
  HTMLPPT_CHECK_EQ(warnings.size(), std::size_t{1});
  HTMLPPT_CHECK(warnings[0].find("99") != std::string::npos);
  // The fallback still fills usable metrics rather than leaving the plan empty.
  HTMLPPT_CHECK(plan.targets[0].screen_width.has_value());
  HTMLPPT_CHECK(*plan.targets[0].screen_width > 0);
}

#endif
