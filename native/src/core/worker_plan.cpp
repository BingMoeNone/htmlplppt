#include "core/worker_plan.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/utf8_path.h"
#include "core/worker_runtime.h"

namespace htmlppt {
namespace {

using json = nlohmann::json;

void SetError(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
}

TriggerMode TriggerModeFromConfig(const std::string& value) {
  return value == "arrive" ? TriggerMode::Arrive : TriggerMode::LeaveForward;
}

std::string ConfigFromTriggerMode(TriggerMode mode) {
  return mode == TriggerMode::Arrive ? "arrive" : "leave-forward";
}

OnFailurePolicy OnFailurePolicyFromConfig(const std::string& value) {
  if (value == "strict") return OnFailurePolicy::Strict;
  if (value == "safe") return OnFailurePolicy::Safe;
  if (value == "legacy") return OnFailurePolicy::Legacy;
  return OnFailurePolicy::SelfHeal;
}

std::string ConfigFromOnFailurePolicy(OnFailurePolicy policy) {
  switch (policy) {
    case OnFailurePolicy::Strict:
      return "strict";
    case OnFailurePolicy::Safe:
      return "safe";
    case OnFailurePolicy::Legacy:
      return "legacy";
    case OnFailurePolicy::SelfHeal:
      break;
  }
  return "self-heal";
}

const TargetConfig* FindSourceTarget(const Config& config, int ordinal) {
  if (ordinal < 1) return nullptr;
  const auto index = static_cast<std::size_t>(ordinal - 1);
  if (index >= config.active.targets.size()) return nullptr;
  return &config.active.targets[index];
}

bool ReadString(const json& object, const char* key, const std::string& path,
                std::string* out, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return true;
  if (!it->is_string()) {
    SetError(error, path + ": expected a string");
    return false;
  }
  *out = it->get<std::string>();
  return true;
}

std::optional<int> ReadIntValue(const json& value, const std::string& path,
                                std::string* error) {
  if (!value.is_number_integer()) {
    SetError(error, path + ": expected an integer");
    return std::nullopt;
  }
  std::int64_t number = 0;
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      SetError(error, path + ": integer is out of range");
      return std::nullopt;
    }
    number = static_cast<std::int64_t>(raw);
  } else {
    number = value.get<std::int64_t>();
  }
  if (number < std::numeric_limits<int>::min() ||
      number > std::numeric_limits<int>::max()) {
    SetError(error, path + ": integer is out of range");
    return std::nullopt;
  }
  return static_cast<int>(number);
}

bool ReadInt(const json& object, const char* key, const std::string& path,
             int* out, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return true;
  const auto value = ReadIntValue(*it, path, error);
  if (!value.has_value()) return false;
  *out = *value;
  return true;
}

bool ReadOptionalInt(const json& object, const char* key,
                     const std::string& path, std::optional<int>* out,
                     std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) {
    *out = std::nullopt;
    return true;
  }
  const auto value = ReadIntValue(*it, path, error);
  if (!value.has_value()) return false;
  *out = *value;
  return true;
}

bool ReadBool(const json& object, const char* key, const std::string& path,
              bool* out, std::string* error) {
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return true;
  if (!it->is_boolean()) {
    SetError(error, path + ": expected a boolean");
    return false;
  }
  *out = it->get<bool>();
  return true;
}

bool ReadTargetPlan(const json& value, const std::string& path,
                    TargetPlan* target, std::string* error) {
  if (!value.is_object()) {
    SetError(error, path + ": expected an object");
    return false;
  }
  if (!ReadInt(value, "slide", path + ".slide", &target->slide, error)) {
    return false;
  }
  if (!ReadString(value, "label", path + ".label", &target->label, error)) {
    return false;
  }
  if (!ReadString(value, "edgeProfile", path + ".edgeProfile",
                  &target->edge_profile, error)) {
    return false;
  }

  std::string trigger = ConfigFromTriggerMode(target->trigger);
  if (!ReadString(value, "trigger", path + ".trigger", &trigger, error)) {
    return false;
  }
  if (trigger != "arrive" && trigger != "leave-forward") {
    SetError(error, path + ".trigger: expected leave-forward or arrive");
    return false;
  }
  target->trigger = TriggerModeFromConfig(trigger);

  std::string on_failure = ConfigFromOnFailurePolicy(target->on_failure);
  if (!ReadString(value, "onFailure", path + ".onFailure", &on_failure, error)) {
    return false;
  }
  if (on_failure != "self-heal" && on_failure != "strict" &&
      on_failure != "safe" && on_failure != "legacy") {
    SetError(error,
             path + ".onFailure: expected self-heal, strict, safe, or legacy");
    return false;
  }
  target->on_failure = OnFailurePolicyFromConfig(on_failure);

  if (!ReadOptionalInt(value, "maxConsecutiveTriggerFailures",
                       path + ".maxConsecutiveTriggerFailures",
                       &target->max_consecutive_trigger_failures, error)) {
    return false;
  }
  std::optional<int> cover_max_hold_ms;
  if (!ReadOptionalInt(value, "coverMaxHoldMs", path + ".coverMaxHoldMs",
                       &cover_max_hold_ms, error)) {
    return false;
  }
  if (cover_max_hold_ms.has_value()) {
    target->cover_max_hold = Duration{*cover_max_hold_ms};
  }
  return ReadBool(value, "enabled", path + ".enabled", &target->enabled, error);
}

bool ReadResolvedTarget(const json& value, const std::string& path,
                        ResolvedTarget* target, std::string* error) {
  if (!value.is_object()) {
    SetError(error, path + ": expected an object");
    return false;
  }
  if (!ReadInt(value, "ordinal", path + ".ordinal", &target->ordinal, error)) {
    return false;
  }
  if (!ReadInt(value, "slide", path + ".slide", &target->slide, error)) {
    return false;
  }
  if (!ReadString(value, "label", path + ".label", &target->label, error)) {
    return false;
  }
  if (!ReadString(value, "htmlPath", path + ".htmlPath", &target->html_path,
                  error)) {
    return false;
  }
  if (!ReadString(value, "query", path + ".query", &target->query, error)) {
    return false;
  }
  if (!ReadString(value, "fragment", path + ".fragment", &target->fragment,
                  error)) {
    return false;
  }
  if (!ReadString(value, "stageName", path + ".stageName",
                  &target->stage_name, error)) {
    return false;
  }
  if (!ReadString(value, "edgeProfile", path + ".edgeProfile",
                  &target->edge_profile, error)) {
    return false;
  }
  target->state = target->slide > 0 ? ResolveState::Resolved
                                    : ResolveState::Disabled;
  return true;
}

nlohmann::ordered_json TargetPlanToJson(const TargetPlan& target) {
  nlohmann::ordered_json result;
  result["slide"] = target.slide;
  result["label"] = target.label;
  result["edgeProfile"] = target.edge_profile;
  result["trigger"] = ConfigFromTriggerMode(target.trigger);
  result["onFailure"] = ConfigFromOnFailurePolicy(target.on_failure);
  if (target.max_consecutive_trigger_failures.has_value()) {
    result["maxConsecutiveTriggerFailures"] =
        *target.max_consecutive_trigger_failures;
  } else {
    result["maxConsecutiveTriggerFailures"] = nullptr;
  }
  if (target.cover_max_hold.has_value()) {
    result["coverMaxHoldMs"] = target.cover_max_hold->count();
  } else {
    result["coverMaxHoldMs"] = nullptr;
  }
  result["enabled"] = target.enabled;
  return result;
}

nlohmann::ordered_json ResolvedTargetToJson(const ResolvedTarget& target) {
  nlohmann::ordered_json result;
  result["ordinal"] = target.ordinal;
  result["slide"] = target.slide;
  result["label"] = target.label;
  result["htmlPath"] = target.html_path;
  result["query"] = target.query;
  result["fragment"] = target.fragment;
  result["stageName"] = target.stage_name;
  result["edgeProfile"] = target.edge_profile;
  return result;
}

} // namespace

WorkerPlan BuildWorkerPlan(const Config& config, const ResolveResult& resolved,
                           int slide_count, std::string run_id) {
  WorkerPlan plan;
  plan.machine.run_id = std::move(run_id);
  plan.machine.slide_count = slide_count;
  plan.deck_utf8 = Utf8Path(ResolveDeckPath(config));

  struct Entry {
    TargetPlan machine;
    ResolvedTarget spec;
  };
  std::vector<Entry> entries;
  entries.reserve(resolved.targets.size());
  for (const auto& spec : resolved.targets) {
    if (spec.state == ResolveState::Disabled || spec.slide <= 0) continue;
    const TargetConfig* source = FindSourceTarget(config, spec.ordinal);
    if (source == nullptr) continue;

    const std::string& trigger =
        source->trigger.empty() ? config.defaults.trigger : source->trigger;
    const std::string& on_failure = source->on_failure.empty()
                                        ? config.defaults.on_failure
                                        : source->on_failure;

    TargetPlan machine;
    machine.slide = spec.slide;
    machine.label = spec.label;
    machine.edge_profile = spec.edge_profile;
    machine.trigger = TriggerModeFromConfig(trigger);
    machine.on_failure = OnFailurePolicyFromConfig(on_failure);
    machine.max_consecutive_trigger_failures =
        source->max_consecutive_trigger_failures;
    if (source->cover.has_value()) {
      machine.cover_max_hold = Duration{source->cover->max_hold_ms};
    } else {
      machine.cover_max_hold = Duration{config.defaults.cover.max_hold_ms};
    }

    entries.push_back(Entry{std::move(machine), spec});
  }

  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& left, const Entry& right) {
                     return left.machine.slide < right.machine.slide;
                   });
  plan.machine.targets.reserve(entries.size());
  plan.targets.reserve(entries.size());
  for (auto& entry : entries) {
    plan.machine.targets.push_back(std::move(entry.machine));
    plan.targets.push_back(std::move(entry.spec));
  }
  return plan;
}

StateMachineConfig BuildStateMachineConfig(const Config& config) {
  StateMachineConfig machine;
  machine.cover_fade = Duration{config.defaults.cover.fade_ms};
  machine.cover_hold = Duration{config.defaults.cover.hold_ms};
  machine.cover_max_hold = Duration{config.defaults.cover.max_hold_ms};
  machine.edge_max_live = Duration{config.defaults.edge.max_live_ms};
  machine.max_consecutive_trigger_failures =
      config.defaults.max_consecutive_trigger_failures;
  machine.default_on_failure =
      OnFailurePolicyFromConfig(config.defaults.on_failure);
  return machine;
}

WorkerHostConfig BuildWorkerHostConfig(const Config& config,
                                       const WorkerHostFacts& facts) {
  const StateMachineConfig machine = BuildStateMachineConfig(config);
  WorkerHostConfig host;
  host.temp_dir = facts.temp_dir;
  host.edge_exe = facts.edge_exe;
  host.edge = config.defaults.edge;
  host.cover.fade = machine.cover_fade;
  host.cover.hold = machine.cover_hold;
  host.cover.max_hold = machine.cover_max_hold;
  host.focus.timeout = machine.focus_timeout;
  host.screen = facts.screen;
  host.wps_pids = facts.wps_pids;
  return host;
}

std::string WorkerPlanToJson(const WorkerPlan& plan) {
  nlohmann::ordered_json root;
  root["runId"] = plan.machine.run_id;
  root["deck"] = plan.deck_utf8;

  nlohmann::ordered_json machine;
  machine["slideCount"] = plan.machine.slide_count;
  machine["targets"] = nlohmann::ordered_json::array();
  for (const auto& target : plan.machine.targets) {
    machine["targets"].push_back(TargetPlanToJson(target));
  }
  root["machine"] = std::move(machine);

  root["targets"] = nlohmann::ordered_json::array();
  for (const auto& target : plan.targets) {
    root["targets"].push_back(ResolvedTargetToJson(target));
  }
  return root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<WorkerPlan> WorkerPlanFromJson(std::string_view text,
                                             std::string* error) {
  const json root = json::parse(text.begin(), text.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    SetError(error, "worker plan: expected a JSON object");
    return std::nullopt;
  }

  WorkerPlan plan;
  if (!ReadString(root, "runId", "runId", &plan.machine.run_id, error)) {
    return std::nullopt;
  }
  if (plan.machine.run_id.empty()) {
    SetError(error, "worker plan: runId is required");
    return std::nullopt;
  }
  if (!ReadString(root, "deck", "deck", &plan.deck_utf8, error)) {
    return std::nullopt;
  }

  const auto machine_it = root.find("machine");
  if (machine_it == root.end() || !machine_it->is_object()) {
    SetError(error, "worker plan: machine is required");
    return std::nullopt;
  }
  const json& machine = *machine_it;
  if (!ReadInt(machine, "slideCount", "machine.slideCount",
               &plan.machine.slide_count, error)) {
    return std::nullopt;
  }
  if (plan.machine.slide_count < 0) {
    SetError(error, "worker plan: machine.slideCount must be >= 0");
    return std::nullopt;
  }

  std::vector<TargetPlan> machine_targets;
  if (const auto targets_it = machine.find("targets");
      targets_it != machine.end()) {
    if (!targets_it->is_array()) {
      SetError(error, "worker plan: machine.targets must be an array");
      return std::nullopt;
    }
    machine_targets.reserve(targets_it->size());
    for (std::size_t i = 0; i < targets_it->size(); ++i) {
      TargetPlan target;
      if (!ReadTargetPlan((*targets_it)[i],
                          "machine.targets[" + std::to_string(i) + "]",
                          &target, error)) {
        return std::nullopt;
      }
      machine_targets.push_back(std::move(target));
    }
  }

  std::vector<ResolvedTarget> targets;
  if (const auto targets_it = root.find("targets"); targets_it != root.end()) {
    if (!targets_it->is_array()) {
      SetError(error, "worker plan: targets must be an array");
      return std::nullopt;
    }
    targets.reserve(targets_it->size());
    for (std::size_t i = 0; i < targets_it->size(); ++i) {
      ResolvedTarget target;
      if (!ReadResolvedTarget((*targets_it)[i],
                              "targets[" + std::to_string(i) + "]", &target,
                              error)) {
        return std::nullopt;
      }
      targets.push_back(std::move(target));
    }
  }

  if (machine_targets.size() != targets.size()) {
    SetError(
        error,
        "worker plan: machine.targets and targets must have the same length");
    return std::nullopt;
  }
  plan.machine.targets = std::move(machine_targets);
  plan.targets = std::move(targets);
  return plan;
}

bool WriteWorkerPlanFile(const WorkerPlan& plan,
                         const std::filesystem::path& path,
                         std::string* error) {
  const std::string text = WorkerPlanToJson(plan);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    SetError(error, "failed to open worker plan file for writing: " +
                        Utf8Path(path));
    return false;
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.put('\n');
  output.close();
  if (!output) {
    SetError(error, "failed to write worker plan file: " + Utf8Path(path));
    return false;
  }
  return true;
}

std::optional<WorkerPlan> ReadWorkerPlanFile(const std::filesystem::path& path,
                                             std::string* error) {
  std::error_code code;
  if (!std::filesystem::is_regular_file(path, code)) {
    SetError(error,
             "worker plan file is missing or not a regular file: " +
                 Utf8Path(path));
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    SetError(error, "failed to open worker plan file for reading: " +
                        Utf8Path(path));
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (input.bad()) {
    SetError(error, "failed to read worker plan file: " + Utf8Path(path));
    return std::nullopt;
  }
  return WorkerPlanFromJson(buffer.str(), error);
}

} // namespace htmlppt
