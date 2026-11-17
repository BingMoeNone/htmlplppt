#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/faults.h"
#include "core/resolve.h"
#include "core/state_machine_types.h"
#include "core/window_system.h"

namespace htmlppt {

// Supervisor-to-worker handoff: the resolved machine view of a run, the
// resolved target specs for the host layer, and the deck path as UTF-8.
struct WorkerPlan {
  RunPlan machine;
  std::vector<ResolvedTarget> targets;
  std::string deck_utf8;
};

// Host facts the config cannot express: where this run's staged files and Edge
// live, the display metrics, and the WPS processes to focus. They are runtime
// facts, never config.
struct WorkerHostFacts {
  std::filesystem::path temp_dir;
  std::filesystem::path edge_exe;
  ScreenMetrics screen;
  std::vector<std::uint32_t> wps_pids;
  // Fault injection (spec 6.6); all off unless the operator asks for them.
  Faults faults;
};

struct WorkerHostConfig;  // defined in core/worker_runtime.h

// Maps Config::defaults onto the state machine's timing and policy knobs. Knobs
// the config cannot express keep the state machine defaults; the focus timeout
// it leaves here is also what the worker host uses.
StateMachineConfig BuildStateMachineConfig(const Config& config);

// Maps the same defaults plus the host facts onto the worker host config. The
// cover timings and focus timeout come from BuildStateMachineConfig so the host
// and the machine cannot drift. Include core/worker_runtime.h where the result
// is used: WorkerHostConfig is only forward declared here.
WorkerHostConfig BuildWorkerHostConfig(const Config& config,
                                       const WorkerHostFacts& facts);

// Maps the resolved configuration onto one TargetPlan per enabled target.
// Resolved targets with slide <= 0 are dropped. Effective trigger, on_failure,
// and cover max-hold fall back to the config defaults when the per-target value
// is absent. Both vectors are sorted by slide so index i always addresses the
// same target.
WorkerPlan BuildWorkerPlan(const Config& config, const ResolveResult& resolved,
                           int slide_count, std::string run_id);

// Serializes the plan as UTF-8 JSON with non-ASCII text written as-is. The
// optional machine-target fields are written as null when unset. Invalid UTF-8
// is replaced rather than reported, so this cannot throw on bad input.
std::string WorkerPlanToJson(const WorkerPlan& plan);

// Parses a plan produced by WorkerPlanToJson. Unknown keys are ignored; a
// missing or empty runId, a negative slideCount, or mismatched
// machine.targets/targets lengths set *error and return nullopt.
std::optional<WorkerPlan> WorkerPlanFromJson(std::string_view json,
                                             std::string* error);

// Writes the plan file as BOM-free UTF-8. Failures are reported through
// *error and the return value; no exception escapes.
bool WriteWorkerPlanFile(const WorkerPlan& plan,
                         const std::filesystem::path& path, std::string* error);

// Reads a plan file. A missing or unreadable file is nullopt with a message
// in *error; no exception escapes.
std::optional<WorkerPlan> ReadWorkerPlanFile(const std::filesystem::path& path,
                                             std::string* error);

} // namespace htmlppt
