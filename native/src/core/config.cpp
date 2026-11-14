#include "core/config.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
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

void AddWarning(std::vector<ConfigIssue>& issues, std::string path,
                std::string message) {
  AddIssue(issues, IssueSeverity::Warning, std::move(path), std::move(message));
}

bool HasErrorIssue(const std::vector<ConfigIssue>& issues) {
  for (const auto& issue : issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
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

std::optional<std::int64_t> ReadInt64Value(const json& value, const std::string& path,
                                           std::vector<ConfigIssue>& issues) {
  if (!value.is_number_integer()) {
    AddError(issues, path, "must be an integer");
    return std::nullopt;
  }
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      AddError(issues, path, "integer is out of int64 range");
      return std::nullopt;
    }
    return static_cast<std::int64_t>(number);
  }
  return value.get<std::int64_t>();
}

std::optional<int> ReadIntValue(const json& value, const std::string& path,
                                std::vector<ConfigIssue>& issues) {
  const auto number = ReadInt64Value(value, path, issues);
  if (!number.has_value()) return std::nullopt;
  if (*number < std::numeric_limits<int>::min() ||
      *number > std::numeric_limits<int>::max()) {
    AddError(issues, path, "integer is out of 32-bit range");
    return std::nullopt;
  }
  return static_cast<int>(*number);
}

std::optional<int> ReadInt(const json& object, const char* key,
                           const std::string& path,
                           std::vector<ConfigIssue>& issues) {
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  return ReadIntValue(*it, FieldPath(path, key), issues);
}

std::optional<std::int64_t> ReadInt64(const json& object, const char* key,
                                      const std::string& path,
                                      std::vector<ConfigIssue>& issues) {
  const auto it = object.find(key);
  if (it == object.end()) return std::nullopt;
  return ReadInt64Value(*it, FieldPath(path, key), issues);
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

std::vector<int> ReadIntArray(const json& object, const char* key,
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
    if (auto number = ReadIntValue(item, IndexPath(FieldPath(path, key), i), issues)) {
      values.push_back(*number);
    }
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
  if (value.contains("args")) {
    result.args = ReadStringArray(value, "args", path, issues);
  }
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
  const std::string monitor_path = FieldPath(path, "monitor");
  const auto monitor = value.find("monitor");
  if (monitor == value.end()) return result;
  if (monitor->is_number_integer()) {
    if (monitor->is_number_unsigned()) {
      const auto index = monitor->get<std::uint64_t>();
      if (index < 1) {
        AddError(issues, monitor_path, "must be >= 1");
      } else {
        result.monitor = std::to_string(index);
      }
    } else {
      const auto index = monitor->get<std::int64_t>();
      if (index < 1) {
        AddError(issues, monitor_path, "must be >= 1");
      } else {
        result.monitor = std::to_string(index);
      }
    }
    return result;
  }
  if (!monitor->is_string()) {
    AddError(issues, monitor_path, "must be a string or an integer");
    return result;
  }
  const std::string text = monitor->get<std::string>();
  if (IsOneOf(text, {"primary", "secondary"})) {
    result.monitor = text;
    return result;
  }
  int index = 0;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, index);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end || index < 1) {
    AddError(issues, monitor_path, "must be primary, secondary, or an index >= 1");
    return result;
  }
  result.monitor = std::to_string(index);
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
  if (value.contains("include")) {
    result.include = ReadStringArray(value, "include", path, issues);
  }
  if (value.contains("exclude")) {
    result.exclude = ReadStringArray(value, "exclude", path, issues);
  }
  if (auto bytes = ReadInt64(value, "maxBytes", path, issues)) result.max_bytes = *bytes;
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
  if (target.slide.has_value() && *target.slide < 1) {
    AddError(issues, FieldPath(path, "slide"), "must be >= 1");
  }
  if (const auto it = value.find("match"); it != value.end()) {
    target.match = ParseMatch(*it, FieldPath(path, "match"), issues);
  }
  if (target.slide.has_value() == target.match.has_value()) {
    AddError(issues, path, "exactly one of slide or match is required");
  }
  if (const auto it = value.find("html"); it == value.end()) {
    AddError(issues, FieldPath(path, "html"), "required string is missing");
  } else if (!it->is_string()) {
    AddError(issues, FieldPath(path, "html"), "must be a string");
  } else {
    target.html = it->get<std::string>();
    if (target.html.empty()) {
      AddError(issues, FieldPath(path, "html"), "must not be empty");
    }
  }
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
  if (const auto it = value.find("onFailure"); it == value.end()) {
    target.on_failure = defaults.on_failure;
  } else if (!it->is_string()) {
    AddError(issues, FieldPath(path, "onFailure"), "must be a string");
  } else {
    target.on_failure = it->get<std::string>();
    if (!IsOneOf(target.on_failure, {"self-heal", "strict", "safe", "legacy"})) {
      AddError(issues, FieldPath(path, "onFailure"),
               "must be self-heal, strict, safe, or legacy");
    }
  }
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
  if (!IsOneOf(target.trigger, {"leave-forward", "arrive"})) {
    AddError(issues, FieldPath(path, "trigger"), "must be leave-forward or arrive");
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
                       const StageConfig& base, std::vector<ConfigIssue>& issues) {
  StageConfig result = base;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return result;
  }
  CheckKnownKeys(value, {"mode", "root"}, path, issues);
  if (auto mode = ReadString(value, "mode", path, issues)) result.mode = *mode;
  if (auto root = ReadString(value, "root", path, issues)) result.root = *root;
  if (!IsOneOf(result.mode, {"file", "tree", "direct"})) {
    AddError(issues, FieldPath(path, "mode"), "must be file, tree, or direct");
  }
  return result;
}

ProfileConfig ParseProfile(const json& value, const std::string& path, int priority,
                           const DefaultsConfig& defaults, const StageConfig& inherited_stage,
                           std::vector<ConfigIssue>& issues) {
  ProfileConfig profile;
  profile.stage = inherited_stage;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return profile;
  }
  CheckKnownKeys(value, {"deck", "stateDir", "stage", "targets"}, path, issues);
  profile.deck = ReadString(value, "deck", path, issues).value_or("");
  profile.state_dir = ReadString(value, "stateDir", path, issues).value_or("");
  if (const auto it = value.find("stage"); it != value.end()) {
    profile.stage = ParseStage(*it, FieldPath(path, "stage"), inherited_stage, issues);
  }
  if (const auto it = value.find("targets"); it != value.end()) {
    profile.targets = ParseTargets(*it, FieldPath(path, "targets"), priority, defaults, issues);
  }
  return profile;
}

LegacyConfig ParseLegacy(const json& value, const std::string& path,
                         const DefaultsConfig& defaults, std::vector<ConfigIssue>& issues) {
  LegacyConfig legacy;
  legacy.present = true;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return legacy;
  }
  CheckKnownKeys(value,
                 {"deckName", "targetSlides", "srcNames", "targetHtmls",
                  "targetProfiles", "targetLabels"},
                 path, issues);
  legacy.deck_name = ReadString(value, "deckName", path, issues).value_or("");
  legacy.target_slides = ReadIntArray(value, "targetSlides", path, issues);
  legacy.src_names = ReadStringArray(value, "srcNames", path, issues);
  legacy.target_htmls = ReadStringArray(value, "targetHtmls", path, issues);
  legacy.target_profiles = ReadStringArray(value, "targetProfiles", path, issues);
  legacy.target_labels = ReadStringArray(value, "targetLabels", path, issues);

  const std::size_t size = legacy.target_slides.size();
  const bool equal_lengths = legacy.src_names.size() == size &&
                             legacy.target_htmls.size() == size &&
                             legacy.target_profiles.size() == size &&
                             legacy.target_labels.size() == size;
  if (!equal_lengths) {
    AddError(issues, path, "legacy arrays must have equal length");
    return legacy;
  }
  if (legacy.deck_name.empty()) AddError(issues, FieldPath(path, "deckName"), "must not be empty");
  for (std::size_t i = 0; i < size; ++i) {
    TargetConfig target;
    target.slide = legacy.target_slides[i];
    target.html = legacy.src_names[i];
    target.stage_name = legacy.target_htmls[i];
    target.edge_profile = legacy.target_profiles[i];
    target.label = legacy.target_labels[i];
    target.trigger = defaults.trigger;
    target.on_failure = defaults.on_failure;
    target.source_priority = 0;
    if (target.html.empty()) {
      AddError(issues, IndexPath(FieldPath(path, "srcNames"), i), "must not be empty");
    }
    if (!target.stage_name.empty() && !IsAscii(target.stage_name)) {
      AddError(issues, IndexPath(FieldPath(path, "targetHtmls"), i), "must be ASCII");
    }
    if (*target.slide < 1) {
      AddError(issues, IndexPath(FieldPath(path, "targetSlides"), i), "must be >= 1");
    }
    legacy.targets.push_back(std::move(target));
  }
  return legacy;
}

nlohmann::ordered_json CoverToJson(const CoverConfig& cover) {
  return {
      {"fadeMs", cover.fade_ms},
      {"holdMs", cover.hold_ms},
      {"maxHoldMs", cover.max_hold_ms},
  };
}

nlohmann::ordered_json EdgeToJson(const EdgeConfig& edge) {
  nlohmann::ordered_json result{
      {"maxLiveMs", edge.max_live_ms},
      {"prewarmProfile", edge.prewarm_profile},
  };
  if (!edge.args.empty()) result["args"] = edge.args;
  return result;
}

nlohmann::ordered_json DisplayToJson(const DisplayConfig& display) {
  return {{"monitor", display.monitor}};
}

nlohmann::ordered_json FocusToJson(const FocusConfig& focus) {
  return {{"strategy", focus.strategy}, {"require", focus.require}};
}

nlohmann::ordered_json AssetsToJson(const AssetsConfig& assets) {
  nlohmann::ordered_json result{
      {"mode", assets.mode},
      {"root", assets.root},
  };
  if (!assets.include.empty()) result["include"] = assets.include;
  if (!assets.exclude.empty()) result["exclude"] = assets.exclude;
  if (assets.max_bytes > 0) result["maxBytes"] = assets.max_bytes;
  return result;
}

nlohmann::ordered_json TargetToJson(const TargetConfig& target) {
  nlohmann::ordered_json result;
  if (target.slide.has_value()) result["slide"] = *target.slide;
  if (target.match.has_value()) {
    nlohmann::ordered_json match;
    if (!target.match->slide_title.empty()) match["slideTitle"] = target.match->slide_title;
    if (!target.match->slide_name.empty()) match["slideName"] = target.match->slide_name;
    match["mode"] = target.match->mode;
    result["match"] = std::move(match);
  }
  result["html"] = target.html;
  if (!target.label.empty()) result["label"] = target.label;
  if (!target.edge_profile.empty()) result["edgeProfile"] = target.edge_profile;
  if (!target.stage_name.empty()) result["stageName"] = target.stage_name;
  result["assets"] = AssetsToJson(target.assets);
  if (!target.html_query.empty()) result["htmlQuery"] = target.html_query;
  result["trigger"] = target.trigger;
  if (target.cover.has_value()) result["cover"] = CoverToJson(*target.cover);
  if (target.edge.has_value()) result["edge"] = EdgeToJson(*target.edge);
  if (target.display.has_value()) result["display"] = DisplayToJson(*target.display);
  if (target.focus.has_value()) result["focus"] = FocusToJson(*target.focus);
  if (!target.on_failure.empty()) result["onFailure"] = target.on_failure;
  if (target.max_consecutive_trigger_failures.has_value()) {
    result["maxConsecutiveTriggerFailures"] = *target.max_consecutive_trigger_failures;
  }
  result["enabled"] = target.enabled;
  if (!target.tags.empty()) result["tags"] = target.tags;
  return result;
}

nlohmann::ordered_json DefaultsToJson(const DefaultsConfig& defaults) {
  return {
      {"trigger", defaults.trigger},
      {"cover", CoverToJson(defaults.cover)},
      {"edge", EdgeToJson(defaults.edge)},
      {"display", DisplayToJson(defaults.display)},
      {"focus", FocusToJson(defaults.focus)},
      {"scheduling", defaults.scheduling},
      {"onFailure", defaults.on_failure},
      {"maxConsecutiveTriggerFailures", defaults.max_consecutive_trigger_failures},
  };
}

nlohmann::ordered_json StageToJson(const StageConfig& stage) {
  return {{"mode", stage.mode}, {"root", stage.root}};
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
    target.trigger = config.defaults.trigger;
    target.on_failure = config.defaults.on_failure;
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
  if (root.find("version") == root.end()) {
    AddError(result.issues, "version", "required integer is missing");
  } else if (auto version = ReadInt(root, "version", "", result.issues)) {
    config.version = *version;
  }
  if (config.version != 1) AddError(result.issues, "version", "only version 1 is supported");
  config.active_profile = ReadString(root, "activeProfile", "", result.issues).value_or("");
  config.top_level.deck = ReadString(root, "deck", "", result.issues).value_or("");
  config.top_level.state_dir = ReadString(root, "stateDir", "", result.issues).value_or("");
  if (const auto it = root.find("stage"); it != root.end()) {
    config.top_level.stage = ParseStage(*it, "stage", StageConfig{}, result.issues);
  }
  if (const auto it = root.find("defaults"); it != root.end()) {
    config.defaults = ParseDefaults(*it, "defaults", config.defaults, result.issues);
  }
  if (const auto it = root.find("targets"); it != root.end()) {
    config.top_level.targets = ParseTargets(*it, "targets", 1, config.defaults, result.issues);
  }
  if (const auto it = root.find("profiles"); it != root.end()) {
    if (!it->is_object()) {
      AddError(result.issues, "profiles", "must be an object");
    } else {
      for (auto profile = it->begin(); profile != it->end(); ++profile) {
        result.available_profiles.push_back(profile.key());
        config.profiles[profile.key()] =
            ParseProfile(*profile, "profiles." + profile.key(), 2, config.defaults,
                         config.top_level.stage, result.issues);
      }
    }
  }
  if (const auto it = root.find("legacy"); it != root.end()) {
    config.legacy = ParseLegacy(*it, "legacy", config.defaults, result.issues);
  }

  const std::string selected =
      overrides.profile.empty() ? config.active_profile : overrides.profile;
  if (config.profiles.empty()) {
    if (!selected.empty()) {
      AddWarning(result.issues, "activeProfile",
                 "no profiles are defined; the active profile was ignored");
    }
    config.active = config.top_level;
  } else {
    const ProfileConfig* chosen = nullptr;
    std::string chosen_name;
    if (config.profiles.size() == 1) {
      // Spec 8.2: a single-profile config ignores --profile.
      chosen_name = config.profiles.begin()->first;
      chosen = &config.profiles.begin()->second;
    } else if (selected.empty()) {
      AddError(result.issues, "activeProfile",
               "multiple profiles require --profile or activeProfile");
    } else {
      const auto profile = config.profiles.find(selected);
      if (profile == config.profiles.end()) {
        AddError(result.issues, "activeProfile", "unknown profile: " + selected);
      } else {
        chosen_name = selected;
        chosen = &profile->second;
      }
    }
    if (chosen != nullptr) {
      config.active_profile = chosen_name;
      config.active = *chosen;
      if (config.active.deck.empty()) config.active.deck = config.top_level.deck;
      if (config.active.state_dir.empty()) {
        config.active.state_dir = config.top_level.state_dir;
      }
      config.active.targets.insert(config.active.targets.end(),
                                   config.top_level.targets.begin(),
                                   config.top_level.targets.end());
    }
  }

  if (config.legacy.present) {
    if (config.active.deck.empty()) config.active.deck = config.legacy.deck_name;
    config.active.targets.insert(config.active.targets.end(),
                                 config.legacy.targets.begin(),
                                 config.legacy.targets.end());
  }

  ApplyOverrides(config, overrides, result.issues);
  if (!HasErrorIssue(result.issues)) {
    result.config = std::move(config);
  }
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

// Serializes the effective configuration as a self-consistent, one-way
// snapshot: profiles are expanded into the active profile, and raw profile or
// legacy definitions are not emitted. This is not a lossless, profile-preserving
// serializer. source_priority is intentionally omitted; precedence is resolved
// by downstream consumers, not by reparsing the snapshot.
std::string ConfigToJson(const Config& config) {
  nlohmann::ordered_json root;
  root["version"] = config.version;
  root["deck"] = config.active.deck;
  if (!config.active.state_dir.empty()) root["stateDir"] = config.active.state_dir;
  root["stage"] = StageToJson(config.active.stage);
  root["defaults"] = DefaultsToJson(config.defaults);
  root["targets"] = nlohmann::ordered_json::array();
  for (const auto& target : config.active.targets) {
    root["targets"].push_back(TargetToJson(target));
  }
  return root.dump(2);
}

} // namespace htmlppt
