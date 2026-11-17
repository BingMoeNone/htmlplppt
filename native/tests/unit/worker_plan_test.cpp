#include "test_main.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "core/config.h"
#include "core/resolve.h"
#include "core/state_machine_types.h"
#include "core/utf8_path.h"
#include "core/window_system.h"
#include "core/worker_plan.h"
#include "core/worker_runtime.h"

using htmlppt::BuildStateMachineConfig;
using htmlppt::BuildWorkerHostConfig;
using htmlppt::BuildWorkerPlan;
using htmlppt::Config;
using htmlppt::Duration;
using htmlppt::OnFailurePolicy;
using htmlppt::ReadWorkerPlanFile;
using htmlppt::ResolveResult;
using htmlppt::ResolvedTarget;
using htmlppt::ScreenMetrics;
using htmlppt::StateMachineConfig;
using htmlppt::TargetPlan;
using htmlppt::TriggerMode;
using htmlppt::WorkerHostConfig;
using htmlppt::WorkerHostFacts;
using htmlppt::WorkerPlan;
using htmlppt::WorkerPlanFromJson;
using htmlppt::WorkerPlanToJson;
using htmlppt::WriteWorkerPlanFile;

namespace {

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::filesystem::path TempDir() {
  static const htmlppt::test::TempDir dir;
  return dir.path();
}

struct ConfigFixture {
  htmlppt::test::TempDir dir;

  ConfigFixture() {
    WriteFile(dir.path() / "deck.pptx", "PK");
    WriteFile(dir.path() / "page3.html", "<html>3</html>");
    WriteFile(dir.path() / "page5.html", "<html>5</html>");
    WriteFile(dir.path() / "page8.html", "<html>8</html>");
  }
};

const htmlppt::test::TempDir& Fixture() {
  static const ConfigFixture fixture;
  return fixture.dir;
}

Config ParseConfig(std::string_view text) {
  const auto result =
      htmlppt::LoadConfigFromString(text, Fixture().path() / "watcher.json", {});
  HTMLPPT_CHECK(result.config.has_value());
  return *result.config;
}

bool HasError(const ResolveResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == htmlppt::IssueSeverity::Error) return true;
  }
  return false;
}

Config LoadTestConfig() {
  return ParseConfig(R"({
    "version": 1,
    "deck": "deck.pptx",
    "defaults": {
      "trigger": "arrive",
      "onFailure": "strict",
      "maxConsecutiveTriggerFailures": 4,
      "cover": { "maxHoldMs": 9000 }
    },
    "targets": [
      {
        "slide": 5,
        "html": "page5.html",
        "label": "Page five",
        "edgeProfile": "profile-five",
        "trigger": "leave-forward",
        "cover": { "maxHoldMs": 7000 },
        "maxConsecutiveTriggerFailures": 2
      },
      { "slide": 8, "html": "page8.html", "enabled": false }
    ]
  })");
}

ResolveResult ResolveTestTargets(const Config& config) {
  htmlppt::ResolveOptions options;
  options.deck_fingerprint = "0123456789abcdef";
  ResolveResult resolved = htmlppt::ResolveTargets(config, nullptr, options);
  HTMLPPT_CHECK(!HasError(resolved));
  return resolved;
}

// Every defaults knob the runtime mappings read, with values distinct from the
// built-in defaults so reading the wrong field cannot pass.
Config LoadMappingConfig() {
  return ParseConfig(R"({
    "version": 1,
    "deck": "deck.pptx",
    "defaults": {
      "trigger": "arrive",
      "onFailure": "safe",
      "maxConsecutiveTriggerFailures": 5,
      "cover": { "fadeMs": 250, "holdMs": 1500, "maxHoldMs": 9000 },
      "edge": { "maxLiveMs": 120000, "prewarmProfile": true,
                "args": ["--kiosk"] }
    }
  })");
}

// CJK label and CJK html/deck paths to prove the round-trip is byte-preserving.
WorkerPlan MakePlanWithEverything() {
  WorkerPlan plan;
  plan.machine.run_id = "run-42";
  plan.machine.slide_count = 12;

  TargetPlan first;
  first.slide = 3;
  first.label = "\xE5\xBC\x80\xE5\x9C\xBA one";
  first.edge_profile = "htmlppt-deck-t01";
  first.trigger = TriggerMode::Arrive;
  first.on_failure = htmlppt::OnFailurePolicy::Strict;
  first.max_consecutive_trigger_failures = 2;
  first.cover_max_hold = Duration{7500};

  TargetPlan second;
  second.slide = 6;
  second.label = "\xE8\xB0\xA2\xE5\xB9\x95";
  second.edge_profile = "htmlppt-deck-t02";
  second.trigger = TriggerMode::LeaveForward;
  second.on_failure = htmlppt::OnFailurePolicy::Safe;
  second.enabled = false;

  ResolvedTarget first_spec;
  first_spec.ordinal = 1;
  first_spec.slide = 3;
  first_spec.label = first.label;
  first_spec.html_path = "/srv/\xE6\xBC\x94\xE7\xA4\xBA one.html";
  first_spec.query = "mode=live&lang=zh";
  first_spec.fragment = "step-2";
  first_spec.stage_name = "target-01-one.html";
  first_spec.edge_profile = first.edge_profile;

  ResolvedTarget second_spec;
  second_spec.ordinal = 2;
  second_spec.slide = 6;
  second_spec.label = second.label;
  second_spec.html_path = "/srv/\xE8\xB0\xA2\xE5\xB9\x95 two.html";
  second_spec.stage_name = "target-02-two.html";
  second_spec.edge_profile = second.edge_profile;

  plan.machine.targets = {first, second};
  plan.targets = {first_spec, second_spec};
  plan.deck_utf8 = "/srv/\xE6\xBC\x94\xE7\xA4\xBA deck.pptx";
  return plan;
}

std::string ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

} // namespace

HTMLPPT_TEST_CASE(worker_plan_maps_resolved_targets_to_machine_targets) {
  const Config config = LoadTestConfig();
  const ResolveResult resolved = ResolveTestTargets(config);
  const WorkerPlan plan = BuildWorkerPlan(config, resolved, 12, "run-1");
  HTMLPPT_CHECK_EQ(plan.machine.run_id, std::string{"run-1"});
  HTMLPPT_CHECK_EQ(plan.machine.slide_count, 12);
  HTMLPPT_CHECK_EQ(plan.machine.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].label, std::string{"Page five"});
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].edge_profile,
                   resolved.targets[0].edge_profile);
  HTMLPPT_CHECK_EQ(plan.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(plan.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(plan.deck_utf8,
                   htmlppt::Utf8Path(htmlppt::ResolveDeckPath(config)));
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].trigger, TriggerMode::LeaveForward);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].on_failure,
                   htmlppt::OnFailurePolicy::Strict);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].max_consecutive_trigger_failures, 2);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].cover_max_hold, Duration{7000});
  HTMLPPT_CHECK(plan.machine.targets[0].enabled);
}

HTMLPPT_TEST_CASE(worker_plan_falls_back_to_config_defaults) {
  Config config = LoadTestConfig();
  const ResolveResult resolved = ResolveTestTargets(config);
  // A Config assembled without config.cpp keeps empty per-target values.
  config.active.targets[0].trigger.clear();
  config.active.targets[0].on_failure.clear();
  config.active.targets[0].max_consecutive_trigger_failures.reset();
  config.active.targets[0].cover.reset();

  const WorkerPlan plan = BuildWorkerPlan(config, resolved, 12, "run-1");
  HTMLPPT_CHECK_EQ(plan.machine.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].trigger, TriggerMode::Arrive);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].on_failure,
                   htmlppt::OnFailurePolicy::Strict);
  HTMLPPT_CHECK(!plan.machine.targets[0].max_consecutive_trigger_failures.has_value());
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].cover_max_hold, Duration{9000});
}

HTMLPPT_TEST_CASE(worker_plan_sorts_both_vectors_by_slide) {
  const Config config = ParseConfig(R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 8, "html": "page8.html", "label": "Eight" },
      { "slide": 3, "html": "page3.html", "label": "Three" }
    ]
  })");
  const ResolveResult resolved = ResolveTestTargets(config);
  HTMLPPT_CHECK_EQ(resolved.targets[0].slide, 8);

  const WorkerPlan plan = BuildWorkerPlan(config, resolved, 12, "run-2");
  HTMLPPT_CHECK_EQ(plan.machine.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(plan.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].slide, 3);
  HTMLPPT_CHECK_EQ(plan.machine.targets[1].slide, 8);
  HTMLPPT_CHECK_EQ(plan.targets[0].label, std::string{"Three"});
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].label, std::string{"Three"});
  HTMLPPT_CHECK_EQ(plan.targets[1].label, std::string{"Eight"});
  HTMLPPT_CHECK_EQ(plan.machine.targets[1].label, std::string{"Eight"});
}

HTMLPPT_TEST_CASE(worker_plan_json_round_trip_preserves_every_field) {
  const WorkerPlan in = MakePlanWithEverything();
  std::string error;
  const auto out = WorkerPlanFromJson(WorkerPlanToJson(in), &error);
  HTMLPPT_CHECK(out.has_value());
  HTMLPPT_CHECK_EQ(out->machine.targets[0].label, in.machine.targets[0].label);
  HTMLPPT_CHECK_EQ(out->targets[0].html_path, in.targets[0].html_path);
  HTMLPPT_CHECK_EQ(out->deck_utf8, in.deck_utf8);
  HTMLPPT_CHECK_EQ(out->machine.run_id, in.machine.run_id);
  HTMLPPT_CHECK_EQ(out->machine.slide_count, in.machine.slide_count);
  HTMLPPT_CHECK_EQ(out->machine.targets.size(), in.machine.targets.size());
  HTMLPPT_CHECK_EQ(out->targets.size(), in.targets.size());
  for (std::size_t i = 0; i < in.machine.targets.size(); ++i) {
    const TargetPlan& expected = in.machine.targets[i];
    const TargetPlan& actual = out->machine.targets[i];
    HTMLPPT_CHECK_EQ(actual.slide, expected.slide);
    HTMLPPT_CHECK_EQ(actual.label, expected.label);
    HTMLPPT_CHECK_EQ(actual.edge_profile, expected.edge_profile);
    HTMLPPT_CHECK(actual.trigger == expected.trigger);
    HTMLPPT_CHECK(actual.on_failure == expected.on_failure);
    HTMLPPT_CHECK_EQ(actual.max_consecutive_trigger_failures,
                     expected.max_consecutive_trigger_failures);
    HTMLPPT_CHECK_EQ(actual.cover_max_hold, expected.cover_max_hold);
    HTMLPPT_CHECK(actual.enabled == expected.enabled);
  }
  for (std::size_t i = 0; i < in.targets.size(); ++i) {
    const ResolvedTarget& expected = in.targets[i];
    const ResolvedTarget& actual = out->targets[i];
    HTMLPPT_CHECK_EQ(actual.ordinal, expected.ordinal);
    HTMLPPT_CHECK_EQ(actual.slide, expected.slide);
    HTMLPPT_CHECK_EQ(actual.label, expected.label);
    HTMLPPT_CHECK_EQ(actual.html_path, expected.html_path);
    HTMLPPT_CHECK_EQ(actual.query, expected.query);
    HTMLPPT_CHECK_EQ(actual.fragment, expected.fragment);
    HTMLPPT_CHECK_EQ(actual.stage_name, expected.stage_name);
    HTMLPPT_CHECK_EQ(actual.edge_profile, expected.edge_profile);
    HTMLPPT_CHECK(actual.state == expected.state);
  }
}

HTMLPPT_TEST_CASE(worker_plan_json_rejects_missing_run_id) {
  std::string error;
  HTMLPPT_CHECK(!WorkerPlanFromJson(R"({"machine":{"slideCount":3}})", &error).has_value());
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(worker_plan_json_rejects_negative_slide_count) {
  std::string error;
  HTMLPPT_CHECK(!WorkerPlanFromJson(
                    R"({"runId":"run-1","machine":{"slideCount":-1}})", &error)
                    .has_value());
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(worker_plan_json_rejects_target_count_mismatch) {
  std::string error;
  HTMLPPT_CHECK(!WorkerPlanFromJson(
                    R"({"runId":"run-1","machine":{"slideCount":3,"targets":[{"slide":3}]},"targets":[]})",
                    &error)
                    .has_value());
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(worker_plan_json_rejects_invalid_json) {
  std::string error;
  HTMLPPT_CHECK(!WorkerPlanFromJson("{", &error).has_value());
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(worker_plan_json_ignores_unknown_keys) {
  std::string error;
  const auto plan = WorkerPlanFromJson(R"({
    "runId": "run-1",
    "deck": "deck.pptx",
    "machine": { "slideCount": 3, "targets": [], "future": 7 },
    "targets": [],
    "future": { "nested": true }
  })", &error);
  HTMLPPT_CHECK(plan.has_value());
  HTMLPPT_CHECK_EQ(plan->machine.run_id, std::string{"run-1"});
  HTMLPPT_CHECK_EQ(plan->machine.slide_count, 3);
  HTMLPPT_CHECK_EQ(plan->deck_utf8, std::string{"deck.pptx"});
}

HTMLPPT_TEST_CASE(worker_plan_file_round_trip_uses_utf8) {
  const auto dir = TempDir();
  const WorkerPlan in = MakePlanWithEverything();
  std::string error;
  HTMLPPT_CHECK(WriteWorkerPlanFile(in, dir / "worker-plan.json", &error));
  const auto out = ReadWorkerPlanFile(dir / "worker-plan.json", &error);
  HTMLPPT_CHECK(out.has_value());
  HTMLPPT_CHECK_EQ(out->targets[0].html_path, in.targets[0].html_path);
  HTMLPPT_CHECK_EQ(WorkerPlanToJson(*out), WorkerPlanToJson(in));

  const std::string bytes = ReadAllBytes(dir / "worker-plan.json");
  HTMLPPT_CHECK(!bytes.empty());
  HTMLPPT_CHECK(bytes.front() == '{');  // no UTF-8 BOM
  HTMLPPT_CHECK(bytes.find("\xE6\xBC\x94\xE7\xA4\xBA") != std::string::npos);
}

HTMLPPT_TEST_CASE(worker_plan_reads_missing_file_with_error) {
  const auto dir = TempDir();
  std::string error;
  const auto out = ReadWorkerPlanFile(dir / "missing.json", &error);
  HTMLPPT_CHECK(!out.has_value());
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(worker_plan_write_reports_directory_target) {
  const auto dir = TempDir();
  std::string error;
  HTMLPPT_CHECK(!WriteWorkerPlanFile(MakePlanWithEverything(), dir, &error));
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(worker_plan_file_io_tolerates_null_error) {
  const auto dir = TempDir();
  HTMLPPT_CHECK(WriteWorkerPlanFile(MakePlanWithEverything(), dir / "plan.json",
                                    nullptr));
  const auto out = ReadWorkerPlanFile(dir / "plan.json", nullptr);
  HTMLPPT_CHECK(out.has_value());
}

HTMLPPT_TEST_CASE(worker_plan_cover_default_matches_state_machine_config) {
  const Config config = ParseConfig(R"({
    "version": 1,
    "deck": "deck.pptx",
    "defaults": { "cover": { "maxHoldMs": 9000 } },
    "targets": [ { "slide": 3, "html": "page3.html", "label": "Three" } ]
  })");
  const ResolveResult resolved = ResolveTestTargets(config);
  const WorkerPlan plan = BuildWorkerPlan(config, resolved, 12, "run-1");
  HTMLPPT_CHECK_EQ(plan.machine.targets.size(), std::size_t{1});
  // The target has no per-target cover override, so the plan falls back to the
  // same config default BuildStateMachineConfig maps. Pinning the equality is
  // what keeps the plan and the machine from silently diverging.
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].cover_max_hold,
                   BuildStateMachineConfig(config).cover_max_hold);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].cover_max_hold, Duration{9000});
}

HTMLPPT_TEST_CASE(worker_plan_maps_defaults_to_state_machine_config) {
  const StateMachineConfig machine =
      BuildStateMachineConfig(LoadMappingConfig());
  HTMLPPT_CHECK_EQ(machine.cover_fade, Duration{250});
  HTMLPPT_CHECK_EQ(machine.cover_hold, Duration{1500});
  HTMLPPT_CHECK_EQ(machine.cover_max_hold, Duration{9000});
  HTMLPPT_CHECK_EQ(machine.edge_max_live, Duration{120000});
  HTMLPPT_CHECK_EQ(machine.max_consecutive_trigger_failures, 5);
  HTMLPPT_CHECK(machine.default_on_failure == OnFailurePolicy::Safe);
  // No config key feeds the focus timeout, so the state machine default stands
  // and BuildWorkerHostConfig must follow it.
  HTMLPPT_CHECK_EQ(machine.focus_timeout, Duration{6000});
}

HTMLPPT_TEST_CASE(worker_plan_maps_defaults_and_host_facts_to_worker_host) {
  const Config config = LoadMappingConfig();
  WorkerHostFacts facts;
  facts.temp_dir = "C:/Temp/run-7";
  facts.edge_exe = "C:/Edge/msedge.exe";
  facts.screen = ScreenMetrics{1920, 1080};
  facts.wps_pids = {10, 20};

  const WorkerHostConfig host = BuildWorkerHostConfig(config, facts);
  HTMLPPT_CHECK_EQ(host.temp_dir, facts.temp_dir);
  HTMLPPT_CHECK_EQ(host.edge_exe, facts.edge_exe);
  HTMLPPT_CHECK_EQ(host.screen.width, 1920);
  HTMLPPT_CHECK_EQ(host.screen.height, 1080);
  HTMLPPT_CHECK_EQ(host.wps_pids, facts.wps_pids);
  HTMLPPT_CHECK_EQ(host.edge.max_live_ms, 120000);
  HTMLPPT_CHECK(host.edge.prewarm_profile);
  HTMLPPT_CHECK_EQ(host.edge.args.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(host.edge.args.front(), std::string{"--kiosk"});
  HTMLPPT_CHECK_EQ(host.cover.fade, Duration{250});
  HTMLPPT_CHECK_EQ(host.cover.hold, Duration{1500});
  HTMLPPT_CHECK_EQ(host.cover.max_hold, Duration{9000});
  HTMLPPT_CHECK_EQ(host.focus.timeout,
                   BuildStateMachineConfig(config).focus_timeout);
  // Host facts are not config: the remaining host timings keep their defaults.
  HTMLPPT_CHECK_EQ(host.focus.interval, Duration{100});
  HTMLPPT_CHECK_EQ(host.poll_interval, Duration{100});
  HTMLPPT_CHECK_EQ(host.return_retry_interval, Duration{2000});
}
