#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/resolve.h"
#include "core/state_machine_types.h"

namespace htmlppt {

// Supervisor-to-worker handoff: the resolved machine view of a run, the
// resolved target specs for the host layer, and the deck path as UTF-8.
struct WorkerPlan {
  RunPlan machine;
  std::vector<ResolvedTarget> targets;
  std::string deck_utf8;
};

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
