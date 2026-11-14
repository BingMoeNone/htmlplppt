#include "core/config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace htmlppt {
namespace {

using json = nlohmann::json;

bool IsOneOf(const std::string& value, std::initializer_list<std::string_view> values) {
  for (const auto candidate : values) {
    if (value == candidate) return true;
  }
  return false;
}

bool IsAscii(const std::string& value) {
  for (unsigned char character : value) {
    if (character > 0x7F) return false;
  }
  return true;
}

void AddIssue(std::vector<ConfigIssue>& issues, IssueSeverity severity,
              std::string path, std::string message) {
  issues.push_back({severity, std::move(path), std::move(message)});
}

void AddError(std::vector<ConfigIssue>& issues, std::string path, std::string message) {
  AddIssue(issues, IssueSeverity::Error, std::move(path), std::move(message));
}

// Reserved for legacy-array migration warnings.
[[maybe_unused]] void AddWarning(std::vector<ConfigIssue>& issues, std::string path,
                                 std::string message) {
  AddIssue(issues, IssueSeverity::Warning, std::move(path), std::move(message));
}

std::string FieldPath(const std::string& parent, const std::string& key) {
  return parent.empty() ? key : parent + "." + key;
}

std::string IndexPath(const std::string& parent, std::size_t index) {
  return parent + "[" + std::to_string(index) + "]";
}

bool KnownKey(std::string_view key, std::initializer_list<std::string_view> allowed) {
  for (const auto candidate : allowed) {
    if (candidate == key) return true;
  }
  return false;
}

void CheckKnownKeys(const json& object, std::initializer_list<std::string_view> allowed,
                    const std::string& path, std::vector<ConfigIssue>& issues) {
  if (!object.is_object()) return;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!KnownKey(it.key(), allowed)) {
      AddError(issues, FieldPath(path, it.key()), "unknown field");
    }
  }
}

std::optional<std::string> ReadString(const json& object, const char* key,
                                      const std::string& path,
                                      std::vector<ConfigIssue>& issues,
                                      bool required = false) {
  const auto it = object.find(key);
  if (it == object.end()) {
    if (required) AddError(issues, FieldPath(path, key), "required string is missing");
    return std::nullopt;
  }
  if (!it->is_string()) {
    AddError(issues, FieldPath(path, key), "must be a string");
    return std::nullopt;
  }
  return it->get<std::string>();
}

std::optional<int> ReadInt(const json& object, const char* key,
                           const std::string& path,
                           std::vector<ConfigIssue>& issues) {
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  if (!it->is_number_integer()) {
    AddError(issues, FieldPath(path, key), "must be an integer");
    return std::nullopt;
  }
  return it->get<int>();
}

std::optional<bool> ReadBool(const json& object, const char* key,
                             const std::string& path,
                             std::vector<ConfigIssue>& issues) {
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  if (!it->is_boolean()) {
    AddError(issues, FieldPath(path, key), "must be a boolean");
    return std::nullopt;
  }
  return it->get<bool>();
}

std::vector<std::string> ReadStringArray(const json& object, const char* key,
                                         const std::string& path,
                                         std::vector<ConfigIssue>& issues) {
  std::vector<std::string> values;
  const auto it = object.find(key);
  if (it == object.end()) return values;
  if (!it->is_array()) {
    AddError(issues, FieldPath(path, key), "must be an array of strings");
    return values;
  }
  for (std::size_t i = 0; i < it->size(); ++i) {
    const auto& item = (*it)[i];
    if (!item.is_string()) {
      AddError(issues, IndexPath(FieldPath(path, key), i), "must be a string");
      continue;
    }
    values.push_back(item.get<std::string>());
  }
  return values;
}

// Reserved for legacy-array migration.
[[maybe_unused]] std::vector<int> ReadIntArray(const json& object, const char* key,
                                               const std::string& path,
                                               std::vector<ConfigIssue>& issues) {
  std::vector<int> values;
  const auto it = object.find(key);
  if (it == object.end()) return values;
  if (!it->is_array()) {
    AddError(issues, FieldPath(path, key), "must be an array of integers");
    return values;
  }
  for (std::size_t i = 0; i < it->size(); ++i) {
    const auto& item = (*it)[i];
    if (!item.is_number_integer()) {
      AddError(issues, IndexPath(FieldPath(path, key), i), "must be an integer");
      continue;
    }
    values.push_back(item.get<int>());
  }
  return values;
}

CoverConfig ParseCover(const json& value, const std::string& path,
                       const CoverConfig& base, std::vector<ConfigIssue>& issues) {
  CoverConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"fadeMs", "holdMs", "maxHoldMs"}, path, issues);
  if (auto number = ReadInt(value, "fadeMs", path, issues)) result.fade_ms = *number;
  if (auto number = ReadInt(value, "holdMs", path, issues)) result.hold_ms = *number;
  if (auto number = ReadInt(value, "maxHoldMs", path, issues)) result.max_hold_ms = *number;
  if (result.fade_ms <= 0) AddError(issues, FieldPath(path, "fadeMs"), "must be > 0");
  if (result.hold_ms < 0) AddError(issues, FieldPath(path, "holdMs"), "must be >= 0");
  if (result.max_hold_ms < result.hold_ms) {
    AddError(issues, FieldPath(path, "maxHoldMs"), "must be >= holdMs");
  }
  return result;
}

EdgeConfig ParseEdge(const json& value, const std::string& path,
                     const EdgeConfig& base, std::vector<ConfigIssue>& issues) {
  EdgeConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"maxLiveMs", "prewarmProfile", "args"}, path, issues);
  if (auto number = ReadInt(value, "maxLiveMs", path, issues)) result.max_live_ms = *number;
  if (auto flag = ReadBool(value, "prewarmProfile", path, issues)) result.prewarm_profile = *flag;
  result.args = ReadStringArray(value, "args", path, issues);
  if (result.max_live_ms <= 0) AddError(issues, FieldPath(path, "maxLiveMs"), "must be > 0");
  return result;
}

DisplayConfig ParseDisplay(const json& value, const std::string& path,
                           const DisplayConfig& base, std::vector<ConfigIssue>& issues) {
  DisplayConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"monitor"}, path, issues);
  if (auto monitor = ReadString(value, "monitor", path, issues)) result.monitor = *monitor;
  if (!IsOneOf(result.monitor, {"primary", "secondary"})) {
    try {
      const int index = std::stoi(result.monitor);
      if (index < 1) AddError(issues, FieldPath(path, "monitor"), "must be >= 1");
    } catch (const std::exception&) {
      AddError(issues, FieldPath(path, "monitor"), "must be primary, secondary, or an index >= 1");
    }
  }
  return result;
}

FocusConfig ParseFocus(const json& value, const std::string& path,
                       const FocusConfig& base, std::vector<ConfigIssue>& issues) {
  FocusConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"strategy", "require"}, path, issues);
  if (auto strategy = ReadString(value, "strategy", path, issues)) result.strategy = *strategy;
  if (auto required = ReadBool(value, "require", path, issues)) result.require = *required;
  if (result.strategy.empty()) AddError(issues, FieldPath(path, "strategy"), "must not be empty");
  return result;
}

AssetsConfig ParseAssets(const json& value, const std::string& path,
                         const AssetsConfig& base, std::vector<ConfigIssue>& issues) {
  AssetsConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"mode", "root", "include", "exclude", "maxBytes"}, path, issues);
  if (auto mode = ReadString(value, "mode", path, issues)) result.mode = *mode;
  if (auto root = ReadString(value, "root", path, issues)) result.root = *root;
  result.include = ReadStringArray(value, "include", path, issues);
  result.exclude = ReadStringArray(value, "exclude", path, issues);
  if (auto bytes = ReadInt(value, "maxBytes", path, issues)) result.max_bytes = *bytes;
  if (!IsOneOf(result.mode, {"file", "tree", "direct"})) {
    AddError(issues, FieldPath(path, "mode"), "must be file, tree, or direct");
  }
  if (result.max_bytes < 0) AddError(issues, FieldPath(path, "maxBytes"), "must be >= 0");
  return result;
}

MatchConfig ParseMatch(const json& value, const std::string& path,
                       std::vector<ConfigIssue>& issues) {
  MatchConfig result;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"slideTitle", "slideName", "mode"}, path, issues);
  result.slide_title = ReadString(value, "slideTitle", path, issues).value_or("");
  result.slide_name = ReadString(value, "slideName", path, issues).value_or("");
  result.mode = ReadString(value, "mode", path, issues).value_or("exact");
  if (result.slide_title.empty() == result.slide_name.empty()) {
    AddError(issues, path, "exactly one of slideTitle or slideName is required");
  }
  if (!IsOneOf(result.mode, {"exact", "prefix", "regex"})) {
    AddError(issues, FieldPath(path, "mode"), "must be exact, prefix, or regex");
  }
  return result;
}

TargetConfig ParseTarget(const json& value, const std::string& path, int priority,
                         const DefaultsConfig& defaults, std::vector<ConfigIssue>& issues) {
  TargetConfig target;
  target.source_priority = priority;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return target;
  }
  CheckKnownKeys(value,
                 {"slide", "match", "html", "label", "edgeProfile", "stageName",
                  "assets", "htmlQuery", "trigger", "cover", "edge", "display",
                  "focus", "onFailure", "maxConsecutiveTriggerFailures", "enabled", "tags"},
                 path, issues);
  if (auto slide = ReadInt(value, "slide", path, issues)) target.slide = *slide;
  if (const auto it = value.find("match"); it != value.end()) {
    target.match = ParseMatch(*it, FieldPath(path, "match"), issues);
  }
  if (target.slide.has_value() == target.match.has_value()) {
    AddError(issues, path, "exactly one of slide or match is required");
  }
  target.html = ReadString(value, "html", path, issues, true).value_or("");
  target.label = ReadString(value, "label", path, issues).value_or("");
  target.edge_profile = ReadString(value, "edgeProfile", path, issues).value_or("");
  target.stage_name = ReadString(value, "stageName", path, issues).value_or("");
  target.html_query = ReadString(value, "htmlQuery", path, issues).value_or("");
  if (const auto it = value.find("assets"); it != value.end()) {
    target.assets = ParseAssets(*it, FieldPath(path, "assets"), target.assets, issues);
  }
  target.trigger = ReadString(value, "trigger", path, issues).value_or(defaults.trigger);
  if (const auto it = value.find("cover"); it != value.end()) {
    target.cover = ParseCover(*it, FieldPath(path, "cover"), defaults.cover, issues);
  }
  if (const auto it = value.find("edge"); it != value.end()) {
    target.edge = ParseEdge(*it, FieldPath(path, "edge"), defaults.edge, issues);
  }
  if (const auto it = value.find("display"); it != value.end()) {
    target.display = ParseDisplay(*it, FieldPath(path, "display"), defaults.display, issues);
  }
  if (const auto it = value.find("focus"); it != value.end()) {
    target.focus = ParseFocus(*it, FieldPath(path, "focus"), defaults.focus, issues);
  }
  target.on_failure = ReadString(value, "onFailure", path, issues).value_or(defaults.on_failure);
  if (auto failures = ReadInt(value, "maxConsecutiveTriggerFailures", path, issues)) {
    target.max_consecutive_trigger_failures = *failures;
  }
  if (auto enabled = ReadBool(value, "enabled", path, issues)) target.enabled = *enabled;
  if (const auto it = value.find("tags"); it != value.end()) {
    if (!it->is_object()) {
      AddError(issues, FieldPath(path, "tags"), "must be an object of strings");
    } else {
      for (auto tag = it->begin(); tag != it->end(); ++tag) {
        if (!tag->is_string()) {
          AddError(issues, FieldPath(FieldPath(path, "tags"), tag.key()), "must be a string");
        } else {
          target.tags[tag.key()] = tag->get<std::string>();
        }
      }
    }
  }
  if (target.html.empty()) AddError(issues, FieldPath(path, "html"), "must not be empty");
  if (!IsOneOf(target.trigger, {"leave-forward", "arrive"})) {
    AddError(issues, FieldPath(path, "trigger"), "must be leave-forward or arrive");
  }
  if (!target.on_failure.empty() &&
      !IsOneOf(target.on_failure, {"self-heal", "strict", "safe", "legacy"})) {
    AddError(issues, FieldPath(path, "onFailure"), "must be self-heal, strict, safe, or legacy");
  }
  if (target.max_consecutive_trigger_failures.has_value() &&
      *target.max_consecutive_trigger_failures < 0) {
    AddError(issues, FieldPath(path, "maxConsecutiveTriggerFailures"), "must be >= 0");
  }
  if (!target.stage_name.empty() && !IsAscii(target.stage_name)) {
    AddError(issues, FieldPath(path, "stageName"), "must be ASCII");
  }
  return target;
}

std::vector<TargetConfig> ParseTargets(const json& value, const std::string& path,
                                       int priority, const DefaultsConfig& defaults,
                                       std::vector<ConfigIssue>& issues) {
  std::vector<TargetConfig> targets;
  if (!value.is_array()) {
    AddError(issues, path, "must be an array");
    return targets;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    targets.push_back(ParseTarget(value[i], IndexPath(path, i), priority, defaults, issues));
  }
  return targets;
}

DefaultsConfig ParseDefaults(const json& value, const std::string& path,
                             const DefaultsConfig& base, std::vector<ConfigIssue>& issues) {
  DefaultsConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value,
                 {"trigger", "cover", "edge", "display", "focus", "scheduling",
                  "onFailure", "maxConsecutiveTriggerFailures"},
                 path, issues);
  result.trigger = ReadString(value, "trigger", path, issues).value_or(result.trigger);
  if (const auto it = value.find("cover"); it != value.end()) {
    result.cover = ParseCover(*it, FieldPath(path, "cover"), result.cover, issues);
  }
  if (const auto it = value.find("edge"); it != value.end()) {
    result.edge = ParseEdge(*it, FieldPath(path, "edge"), result.edge, issues);
  }
  if (const auto it = value.find("display"); it != value.end()) {
    result.display = ParseDisplay(*it, FieldPath(path, "display"), result.display, issues);
  }
  if (const auto it = value.find("focus"); it != value.end()) {
    result.focus = ParseFocus(*it, FieldPath(path, "focus"), result.focus, issues);
  }
  result.scheduling = ReadString(value, "scheduling", path, issues).value_or(result.scheduling);
  result.on_failure = ReadString(value, "onFailure", path, issues).value_or(result.on_failure);
  if (auto failures = ReadInt(value, "maxConsecutiveTriggerFailures", path, issues)) {
    result.max_consecutive_trigger_failures = *failures;
  }
  if (!IsOneOf(result.trigger, {"leave-forward", "arrive"})) {
    AddError(issues, FieldPath(path, "trigger"), "must be leave-forward or arrive");
  }
  if (!IsOneOf(result.scheduling, {"adaptive", "aggressive", "balanced", "safe"})) {
    AddError(issues, FieldPath(path, "scheduling"),
             "must be adaptive, aggressive, balanced, or safe");
  }
  if (!IsOneOf(result.on_failure, {"self-heal", "strict", "safe", "legacy"})) {
    AddError(issues, FieldPath(path, "onFailure"),
             "must be self-heal, strict, safe, or legacy");
  }
  if (result.max_consecutive_trigger_failures < 0) {
    AddError(issues, FieldPath(path, "maxConsecutiveTriggerFailures"), "must be >= 0");
  }
  return result;
}

StageConfig ParseStage(const json& value, const std::string& path,
                       std::vector<ConfigIssue>& issues) {
  StageConfig result;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"mode", "root"}, path, issues);
  result.mode = ReadString(value, "mode", path, issues).value_or(result.mode);
  result.root = ReadString(value, "root", path, issues).value_or("");
  if (!IsOneOf(result.mode, {"file", "tree", "direct"})) {
    AddError(issues, FieldPath(path, "mode"), "must be file, tree, or direct");
  }
  return result;
}

void ApplyOverrides(Config& config, const CliConfigOverrides& overrides,
                    std::vector<ConfigIssue>& issues) {
  if (overrides.stage_mode.has_value()) {
    if (!IsOneOf(*overrides.stage_mode, {"file", "tree", "direct"})) {
      AddError(issues, "stage.mode", "must be file, tree, or direct");
    } else {
      config.active.stage.mode = *overrides.stage_mode;
    }
  }
  if (overrides.state_dir.has_value()) {
    config.active.state_dir = *overrides.state_dir;
  }
  if (overrides.scheduling.has_value()) {
    if (!IsOneOf(*overrides.scheduling, {"adaptive", "aggressive", "balanced", "safe"})) {
      AddError(issues, "scheduling", "must be adaptive, aggressive, balanced, or safe");
    } else {
      config.defaults.scheduling = *overrides.scheduling;
    }
  }
  for (const auto& [slide, html] : overrides.map_overrides) {
    TargetConfig target;
    target.slide = slide;
    target.html = html;
    target.source_priority = 3;
    if (slide < 1) AddError(issues, "map", "slide must be >= 1");
    if (html.empty()) AddError(issues, "map", "html must not be empty");
    config.active.targets.push_back(std::move(target));
  }
}

} // namespace

ConfigLoadResult LoadConfigFromString(std::string_view text,
                                      const std::filesystem::path& source_path,
                                      const CliConfigOverrides& overrides) {
  ConfigLoadResult result;
  json root;
  try {
    root = json::parse(text.begin(), text.end());
  } catch (const json::parse_error& error) {
    AddError(result.issues, "$", std::string("JSON parse error: ") + error.what());
    return result;
  }
  if (!root.is_object()) {
    AddError(result.issues, "$", "root must be an object");
    return result;
  }

  // Field paths are root-relative, e.g. "targets[0].html" or "defaults.trigger".
  CheckKnownKeys(root,
                 {"version", "activeProfile", "deck", "stateDir", "stage",
                  "defaults", "targets", "profiles", "legacy"},
                 "", result.issues);

  Config config;
  config.source_path = source_path;
  if (auto version = ReadInt(root, "version", "", result.issues)) {
    config.version = *version;
  } else {
    AddError(result.issues, "version", "required integer is missing");
  }
  if (config.version != 1) AddError(result.issues, "version", "only version 1 is supported");
  config.active_profile = ReadString(root, "activeProfile", "", result.issues).value_or("");
  config.top_level.deck = ReadString(root, "deck", "", result.issues).value_or("");
  config.top_level.state_dir = ReadString(root, "stateDir", "", result.issues).value_or("");
  if (const auto it = root.find("stage"); it != root.end()) {
    config.top_level.stage = ParseStage(*it, "stage", result.issues);
  }
  if (const auto it = root.find("defaults"); it != root.end()) {
    config.defaults = ParseDefaults(*it, "defaults", config.defaults, result.issues);
  }
  if (const auto it = root.find("targets"); it != root.end()) {
    config.top_level.targets = ParseTargets(*it, "targets", 1, config.defaults, result.issues);
  }

  config.active = config.top_level;
  ApplyOverrides(config, overrides, result.issues);
  result.config = std::move(config);
  return result;
}

ConfigLoadResult LoadConfigFile(const std::filesystem::path& path,
                                const CliConfigOverrides& overrides) {
  ConfigLoadResult result;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    AddError(result.issues, "$", "cannot open config file: " + path.string());
    return result;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return LoadConfigFromString(buffer.str(), path, overrides);
}

} // namespace htmlppt
