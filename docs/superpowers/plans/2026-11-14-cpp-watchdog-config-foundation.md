# C++ Watchdog Config Foundation Implementation Plan (Plan 2A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the typed configuration foundation for the C++ watchdog: JSON parsing, legacy six-array migration, profiles and precedence metadata, validation with field-path errors, canonical serialization, and the embedded JSON Schema.

**Architecture:** `core/config` owns the typed configuration model, JSON validation, legacy/profile merging, and canonical serialization. `core/config_schema` owns the embedded JSON Schema document. The code stays platform-neutral and is consumed by Plan 2B's deck-info/resolve/CLI layer and Plan 7's config UI.

**Tech Stack:** C++20, CMake/CTest, nlohmann/json 3.11.3, the Plan 1 `htmlppt_core` library and test harness.

## Plan Split

This document is Plan 2A. Plan 2B will cover `DeckInfoProvider`, target resolution (slide index/title), HTML path/query handling, auto stage/Edge-profile names, and the `--validate` / `--resolve` / `--print-config` / `--print-schema` CLI. Splitting keeps each plan independently testable: Plan 2A produces a validated config model and serializer; Plan 2B consumes it.

## Global Constraints

- Target Windows 10/11 x64; Plan 2 must build and test on Linux/WSL.
- C++20; MSVC `/W4 /permissive- /utf-8`; GCC/Clang `-Wall -Wextra -Wpedantic`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code lives under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only. Configuration content is UTF-8 at runtime.
- `core/` must not include Windows-specific headers. The only Windows API use is in `native/src/app/main.cpp`.
- Existing tests (21 unit cases, CTest 3/3, `npm test` 3/3) must stay green.
- Every task ends with a commit; never push.
- Preserve the Plan 1 interfaces: `htmlppt::Duration`, `RetryPolicy`, `Journal`, `State`, `ThreadRole`, `ThreadTier`.

## Scope

Plan 2A implements:

- `watcher.json` parsing for `version`, `activeProfile`, `deck`, `stateDir`, `stage`, `defaults`, `targets`, `profiles`, `legacy`.
- Legacy six-array migration with equal-length validation.
- Profile selection and precedence metadata: CLI map > profile targets > top-level targets > legacy targets.
- Validation with field-path errors, including unknown fields and invalid enum-like values.
- Canonical `ConfigToJson` serialization for `--print-config` and round-trip tests.
- Embedded JSON Schema via `ConfigSchema()`.

Not in Plan 2A: deck-info loading, slide/title resolution, HTML path/query handling, auto stage/Edge-profile names, the CLI executable wiring, WPS COM deck reading, native config UI, staging/copying assets, Edge launch, overlay/focus, state machine. These are Plan 2B, Plan 5, or Plan 7.

## File Structure

```text
native/
  src/
    core/
      config.h                 # typed config model + load/serialize API
      config.cpp               # JSON parsing, legacy/profiles, precedence, serialization
      config_schema.h          # ConfigSchema() declaration
      config_schema.cpp        # embedded JSON Schema document
  tests/
    unit/
      config_test.cpp
      config_schema_test.cpp
```

`native/CMakeLists.txt` needs no changes: the existing `CONFIGURE_DEPENDS` globs pick up new core sources and tests.

---

## Task 1: Config types and top-level JSON parsing

**Files:**
- Create: `native/src/core/config.h`
- Create: `native/src/core/config.cpp`
- Create: `native/tests/unit/config_test.cpp`

**Interfaces:**
- Produces: `htmlppt::Config`, `DefaultsConfig`, `ProfileConfig`, `TargetConfig`, `LegacyConfig`, `ConfigIssue`, `ConfigLoadResult`, `CliConfigOverrides`
- Produces: `LoadConfigFromString`, `LoadConfigFile`
- Produces: `EffectiveProfile(const Config&)`, `EffectiveTargets(const Config&)`

- [ ] **Step 1: Write the failing minimal-config tests**

Create `native/tests/unit/config_test.cpp`:

```cpp
#include "test_main.h"

#include <string>

#include "core/config.h"

using htmlppt::ConfigLoadResult;
using htmlppt::IssueSeverity;
using htmlppt::LoadConfigFromString;

namespace {

ConfigLoadResult Parse(std::string_view text) {
  return LoadConfigFromString(text, std::filesystem::path{"watcher.json"}, {});
}

bool HasError(const ConfigLoadResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == IssueSeverity::Error) {
      return true;
    }
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(config_parses_minimal) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [
      { "slide": 5, "html": "page1.html" }
    ]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(*result.config->active.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(result.config->active.targets[0].html, std::string{"page1.html"});
}

HTMLPPT_TEST_CASE(config_parses_nested_target_and_defaults) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": {
      "trigger": "arrive",
      "cover": { "fadeMs": 200, "holdMs": 500, "maxHoldMs": 8000 },
      "edge": { "maxLiveMs": 120000, "prewarmProfile": true },
      "scheduling": "aggressive",
      "maxConsecutiveTriggerFailures": 4
    },
    "targets": [{
      "match": { "slideTitle": "Audit", "mode": "exact" },
      "html": "audit.html",
      "edgeProfile": "tef-live-audit",
      "stageName": "audit.html",
      "cover": { "maxHoldMs": 12000 },
      "enabled": false,
      "tags": { "section": "audit" }
    }]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& config = *result.config;
  HTMLPPT_CHECK_EQ(config.defaults.trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(config.defaults.cover.fade_ms, 200);
  HTMLPPT_CHECK_EQ(config.defaults.cover.hold_ms, 500);
  HTMLPPT_CHECK_EQ(config.defaults.cover.max_hold_ms, 8000);
  HTMLPPT_CHECK_EQ(config.defaults.edge.max_live_ms, 120000);
  HTMLPPT_CHECK(config.defaults.edge.prewarm_profile);
  HTMLPPT_CHECK_EQ(config.defaults.scheduling, std::string{"aggressive"});
  HTMLPPT_CHECK_EQ(config.defaults.max_consecutive_trigger_failures, 4);
  const auto& target = config.active.targets[0];
  HTMLPPT_CHECK(target.match.has_value());
  HTMLPPT_CHECK_EQ(target.match->slide_title, std::string{"Audit"});
  HTMLPPT_CHECK_EQ(target.cover->max_hold_ms, 12000);
  HTMLPPT_CHECK(!target.enabled);
  HTMLPPT_CHECK_EQ(target.tags.at("section"), std::string{"audit"});
}

HTMLPPT_TEST_CASE(config_rejects_unknown_fields) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "a.html", "mystery": true }]
  })");
  HTMLPPT_CHECK(HasError(result));
  bool found = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "targets[0].mystery") {
      found = true;
    }
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(config_rejects_invalid_enum_and_missing_html) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "", "trigger": "teleport" }]
  })");
  HTMLPPT_CHECK(HasError(result));
  bool trigger_error = false;
  bool html_error = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "targets[0].trigger") trigger_error = true;
    if (issue.path == "targets[0].html") html_error = true;
  }
  HTMLPPT_CHECK(trigger_error);
  HTMLPPT_CHECK(html_error);
}

HTMLPPT_TEST_CASE(config_rejects_bad_json) {
  const auto result = Parse(R"({ "version": 1, )");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_cli_map_override_is_appended) {
  htmlppt::CliConfigOverrides overrides;
  overrides.map_overrides[7] = "override.html";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 5, "html": "page1.html" }]
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(*result.config->active.targets.back().slide, 7);
  HTMLPPT_CHECK_EQ(result.config->active.targets.back().html, std::string{"override.html"});
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/config.h` does not exist.

- [ ] **Step 3: Implement the config model and parser**

Create `native/src/core/config.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace htmlppt {

struct CoverConfig {
  int fade_ms = 300;
  int hold_ms = 1000;
  int max_hold_ms = 10000;
};

struct EdgeConfig {
  int max_live_ms = 600000;
  bool prewarm_profile = false;
  std::vector<std::string> args;
};

struct DisplayConfig {
  std::string monitor = "primary";
};

struct FocusConfig {
  std::string strategy = "auto";
  bool require = true;
};

struct AssetsConfig {
  std::string mode = "file";
  std::string root;
  std::vector<std::string> include;
  std::vector<std::string> exclude;
  std::int64_t max_bytes = 0;
};

struct MatchConfig {
  std::string slide_title;
  std::string slide_name;
  std::string mode = "exact";
};

struct TargetConfig {
  std::optional<int> slide;
  std::optional<MatchConfig> match;
  std::string html;
  std::string label;
  std::string edge_profile;
  std::string stage_name;
  AssetsConfig assets;
  std::string html_query;
  std::string trigger = "leave-forward";
  std::optional<CoverConfig> cover;
  std::optional<EdgeConfig> edge;
  std::optional<DisplayConfig> display;
  std::optional<FocusConfig> focus;
  std::string on_failure;
  std::optional<int> max_consecutive_trigger_failures;
  bool enabled = true;
  std::map<std::string, std::string> tags;
  // Internal precedence: legacy=0, top-level=1, profile=2, CLI map=3.
  int source_priority = 0;
};

struct DefaultsConfig {
  std::string trigger = "leave-forward";
  CoverConfig cover;
  EdgeConfig edge;
  DisplayConfig display;
  FocusConfig focus;
  std::string scheduling = "adaptive";
  std::string on_failure = "self-heal";
  int max_consecutive_trigger_failures = 3;
};

struct StageConfig {
  std::string mode = "file";
  std::string root;
};

struct ProfileConfig {
  std::string deck;
  std::string state_dir;
  StageConfig stage;
  std::vector<TargetConfig> targets;
};

struct LegacyConfig {
  bool present = false;
  std::string deck_name;
  std::vector<int> target_slides;
  std::vector<std::string> src_names;
  std::vector<std::string> target_htmls;
  std::vector<std::string> target_profiles;
  std::vector<std::string> target_labels;
  std::vector<TargetConfig> targets;
};

struct Config {
  int version = 1;
  std::string active_profile;
  DefaultsConfig defaults;
  ProfileConfig top_level;
  std::map<std::string, ProfileConfig> profiles;
  LegacyConfig legacy;
  ProfileConfig active;
  std::filesystem::path source_path;
};

enum class IssueSeverity {
  Error,
  Warning,
};

struct ConfigIssue {
  IssueSeverity severity = IssueSeverity::Error;
  std::string path;
  std::string message;
};

struct ConfigLoadResult {
  std::optional<Config> config;
  std::vector<std::string> available_profiles;
  std::vector<ConfigIssue> issues;
};

struct CliConfigOverrides {
  std::string profile;
  std::optional<std::string> stage_mode;
  std::optional<std::string> state_dir;
  std::optional<std::string> scheduling;
  bool allow_outside_folder = false;
  std::map<int, std::string> map_overrides;
};

ConfigLoadResult LoadConfigFromString(std::string_view text,
                                      const std::filesystem::path& source_path,
                                      const CliConfigOverrides& overrides = {});
ConfigLoadResult LoadConfigFile(const std::filesystem::path& path,
                                const CliConfigOverrides& overrides = {});

inline const ProfileConfig& EffectiveProfile(const Config& config) {
  return config.active;
}

inline const std::vector<TargetConfig>& EffectiveTargets(const Config& config) {
  return config.active.targets;
}

} // namespace htmlppt
```

Create `native/src/core/config.cpp` with the helpers and Task 1 parsing. Use the following structure:

```cpp
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

void AddWarning(std::vector<ConfigIssue>& issues, std::string path, std::string message) {
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
```

This Task 1 version intentionally ignores `profiles` and `legacy`; Task 2 extends the same function.

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/htmlppt_core_tests
```

Expected: the six new config tests pass; existing 21 tests still pass; no warnings.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/config.h native/src/core/config.cpp native/tests/unit/config_test.cpp
git commit -m "feat: add typed config model and JSON parser"
```

---

## Task 2: Legacy arrays, profiles, precedence, and config serialization

**Files:**
- Modify: `native/src/core/config.h` (declare `ConfigToJson`)
- Modify: `native/src/core/config.cpp` (add `ParseProfile`, `ParseLegacy`, profile selection, canonical serialization; replace `LoadConfigFromString`)
- Modify: `native/tests/unit/config_test.cpp`

**Interfaces:**
- Produces: legacy six-array conversion, profile selection, precedence metadata, `ConfigToJson`
- Preserves: Task 1's `LoadConfigFromString` signature and `ConfigLoadResult`

- [ ] **Step 1: Add the failing legacy/profile/serialization tests**

Append to `native/tests/unit/config_test.cpp`:

```cpp
HTMLPPT_TEST_CASE(config_migrates_legacy_arrays) {
  const auto result = Parse(R"({
    "version": 1,
    "legacy": {
      "deckName": "deck-live.pptx",
      "targetSlides": [5, 6],
      "srcNames": ["page1.html", "page2.html"],
      "targetHtmls": ["page1.html", "page2.html"],
      "targetProfiles": ["tef-live-page1", "tef-live-page2"],
      "targetLabels": ["Page1", "Page2"]
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& config = *result.config;
  HTMLPPT_CHECK_EQ(config.active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(config.active.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(*config.active.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(config.active.targets[0].html, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].stage_name, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].edge_profile, std::string{"tef-live-page1"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].label, std::string{"Page1"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].source_priority, 0);
}

HTMLPPT_TEST_CASE(config_rejects_unequal_legacy_arrays) {
  const auto result = Parse(R"({
    "version": 1,
    "legacy": {
      "deckName": "deck-live.pptx",
      "targetSlides": [5, 6],
      "srcNames": ["page1.html"],
      "targetHtmls": ["page1.html", "page2.html"],
      "targetProfiles": ["tef-live-page1", "tef-live-page2"],
      "targetLabels": ["Page1", "Page2"]
    }
  })");
  HTMLPPT_CHECK(HasError(result));
}

HTMLPPT_TEST_CASE(config_selects_single_profile_automatically) {
  const auto result = Parse(R"({
    "version": 1,
    "profiles": {
      "competition": {
        "deck": "deck-live.pptx",
        "targets": [{ "slide": 5, "html": "page1.html" }]
      }
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active_profile, std::string{"competition"});
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{1});
}

HTMLPPT_TEST_CASE(config_requires_profile_when_multiple_exist) {
  const auto result = Parse(R"({
    "version": 1,
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })");
  HTMLPPT_CHECK(HasError(result));
  bool found = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "activeProfile") found = true;
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(config_cli_profile_overrides_active_profile) {
  htmlppt::CliConfigOverrides overrides;
  overrides.profile = "b";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "activeProfile": "a",
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active_profile, std::string{"b"});
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"b.pptx"});
}

HTMLPPT_TEST_CASE(config_precedence_metadata_is_recorded) {
  htmlppt::CliConfigOverrides overrides;
  overrides.map_overrides[5] = "cli.html";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "activeProfile": "a",
    "targets": [{ "slide": 5, "html": "top.html" }],
    "profiles": {
      "a": {
        "deck": "a.pptx",
        "targets": [{ "slide": 5, "html": "profile.html" }]
      }
    },
    "legacy": {
      "deckName": "a.pptx",
      "targetSlides": [5],
      "srcNames": ["legacy.html"],
      "targetHtmls": ["legacy.html"],
      "targetProfiles": ["tef-live-legacy"],
      "targetLabels": ["Legacy"]
    }
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& targets = result.config->active.targets;
  HTMLPPT_CHECK_EQ(targets.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(targets[0].source_priority, 2);  // profile
  HTMLPPT_CHECK_EQ(targets[1].source_priority, 1);  // top-level
  HTMLPPT_CHECK_EQ(targets[2].source_priority, 0);  // legacy
  HTMLPPT_CHECK_EQ(targets[3].source_priority, 3);  // CLI map
}

HTMLPPT_TEST_CASE(config_print_round_trips) {
  const auto first = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": { "trigger": "arrive", "cover": { "maxHoldMs": 9000 } },
    "targets": [{
      "slide": 5,
      "html": "page1.html",
      "label": "Page1",
      "edgeProfile": "tef-live-page1",
      "stageName": "page1.html",
      "cover": { "holdMs": 600 }
    }]
  })");
  HTMLPPT_CHECK(first.config.has_value());
  HTMLPPT_CHECK(!HasError(first));
  const std::string serialized = htmlppt::ConfigToJson(*first.config);
  const auto second = LoadConfigFromString(
      serialized, std::filesystem::path{"watcher.json"}, {});
  HTMLPPT_CHECK(second.config.has_value());
  HTMLPPT_CHECK(!HasError(second));
  HTMLPPT_CHECK_EQ(second.config->active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(second.config->defaults.trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(second.config->defaults.cover.max_hold_ms, 9000);
  HTMLPPT_CHECK_EQ(second.config->active.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(second.config->active.targets[0].cover->hold_ms, 600);
  HTMLPPT_CHECK_EQ(second.config->active.targets[0].edge_profile,
                   std::string{"tef-live-page1"});
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `ConfigToJson` is not declared and profiles/legacy are not parsed.

- [ ] **Step 3: Add serialization declaration**

Modify `native/src/core/config.h` by adding after `LoadConfigFile`:

```cpp
std::string ConfigToJson(const Config& config);
```

- [ ] **Step 4: Implement profiles, legacy, precedence, and serialization**

In `native/src/core/config.cpp`, add the following helpers inside the anonymous namespace (after `ParseStage`):

```cpp
ProfileConfig ParseProfile(const json& value, const std::string& path, int priority,
                           const DefaultsConfig& defaults, std::vector<ConfigIssue>& issues) {
  ProfileConfig profile;
  if (!value.is_object()) {
    AddError(issues, path, "must be an object");
    return profile;
  }
  CheckKnownKeys(value, {"deck", "stateDir", "stage", "targets"}, path, issues);
  profile.deck = ReadString(value, "deck", path, issues).value_or("");
  profile.state_dir = ReadString(value, "stateDir", path, issues).value_or("");
  if (const auto it = value.find("stage"); it != value.end()) {
    profile.stage = ParseStage(*it, FieldPath(path, "stage"), issues);
  }
  if (const auto it = value.find("targets"); it != value.end()) {
    profile.targets = ParseTargets(*it, FieldPath(path, "targets"), priority, defaults, issues);
  }
  return profile;
}

LegacyConfig ParseLegacy(const json& value, const std::string& path,
                         std::vector<ConfigIssue>& issues) {
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
    target.source_priority = 0;
    if (target.html.empty()) {
      AddError(issues, IndexPath(FieldPath(path, "srcNames"), i), "must not be empty");
    }
    if (*target.slide < 1) {
      AddError(issues, IndexPath(FieldPath(path, "targetSlides"), i), "must be >= 1");
    }
    legacy.targets.push_back(std::move(target));
  }
  return legacy;
}
```

Add the serialization helpers after the anonymous namespace helpers (still inside the anonymous namespace):

```cpp
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
```

Replace the Task 1 `LoadConfigFromString` implementation with this complete version:

```cpp
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
  if (const auto it = root.find("profiles"); it != root.end()) {
    if (!it->is_object()) {
      AddError(result.issues, "profiles", "must be an object");
    } else {
      for (auto profile = it->begin(); profile != it->end(); ++profile) {
        result.available_profiles.push_back(profile.key());
        config.profiles[profile.key()] =
            ParseProfile(*profile, "profiles." + profile.key(), 2, config.defaults,
                         result.issues);
      }
    }
  }
  if (const auto it = root.find("legacy"); it != root.end()) {
    config.legacy = ParseLegacy(*it, "legacy", result.issues);
  }

  std::string selected = overrides.profile.empty() ? config.active_profile : overrides.profile;
  if (config.profiles.empty()) {
    if (!selected.empty()) {
      AddWarning(result.issues, "activeProfile",
                 "no profiles are defined; the active profile was ignored");
    }
    config.active = config.top_level;
  } else {
    if (selected.empty()) {
      if (config.profiles.size() == 1) {
        selected = config.profiles.begin()->first;
      } else {
        AddError(result.issues, "activeProfile",
                 "multiple profiles require --profile or activeProfile");
      }
    }
    if (!selected.empty()) {
      const auto chosen = config.profiles.find(selected);
      if (chosen == config.profiles.end()) {
        AddError(result.issues, "activeProfile", "unknown profile: " + selected);
      } else {
        config.active_profile = selected;
        config.active = chosen->second;
        if (config.active.deck.empty()) config.active.deck = config.top_level.deck;
        if (config.active.state_dir.empty()) {
          config.active.state_dir = config.top_level.state_dir;
        }
        if (config.active.stage.root.empty() && !config.top_level.stage.root.empty()) {
          config.active.stage = config.top_level.stage;
        }
        config.active.targets.insert(config.active.targets.end(),
                                     config.top_level.targets.begin(),
                                     config.top_level.targets.end());
      }
    }
  }

  if (config.legacy.present) {
    if (config.active.deck.empty()) config.active.deck = config.legacy.deck_name;
    config.active.targets.insert(config.active.targets.end(),
                                 config.legacy.targets.begin(),
                                 config.legacy.targets.end());
  }

  ApplyOverrides(config, overrides, result.issues);
  result.config = std::move(config);
  return result;
}
```

Add `ConfigToJson` after `LoadConfigFile`:

```cpp
std::string ConfigToJson(const Config& config) {
  nlohmann::ordered_json root;
  root["version"] = config.version;
  if (!config.active_profile.empty()) root["activeProfile"] = config.active_profile;
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
```

- [ ] **Step 5: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/htmlppt_core_tests
```

Expected: all previous tests plus the new legacy/profile/serialization tests pass; no warnings.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/config.h native/src/core/config.cpp native/tests/unit/config_test.cpp
git commit -m "feat: add legacy migration, profiles, and config serialization"
```

---

## Task 3: Embedded JSON Schema

**Files:**
- Create: `native/src/core/config_schema.h`
- Create: `native/src/core/config_schema.cpp`
- Create: `native/tests/unit/config_schema_test.cpp`

**Interfaces:**
- Produces: `std::string_view htmlppt::ConfigSchema()`

- [ ] **Step 1: Write the failing schema tests**

Create `native/tests/unit/config_schema_test.cpp`:

```cpp
#include "test_main.h"

#include <string>

#include <nlohmann/json.hpp>

#include "core/config_schema.h"

HTMLPPT_TEST_CASE(config_schema_is_valid_json) {
  const auto schema = nlohmann::json::parse(htmlppt::ConfigSchema());
  HTMLPPT_CHECK(schema.is_object());
  HTMLPPT_CHECK_EQ(schema["$schema"].get<std::string>(),
                   std::string{"https://json-schema.org/draft/2020-12/schema"});
  HTMLPPT_CHECK_EQ(schema["title"].get<std::string>(),
                   std::string{"htmlppt watchdog config"});
}

HTMLPPT_TEST_CASE(config_schema_declares_core_properties) {
  const auto schema = nlohmann::json::parse(htmlppt::ConfigSchema());
  HTMLPPT_CHECK(schema["properties"].contains("version"));
  HTMLPPT_CHECK(schema["properties"].contains("deck"));
  HTMLPPT_CHECK(schema["properties"].contains("targets"));
  HTMLPPT_CHECK(schema["properties"].contains("profiles"));
  HTMLPPT_CHECK(schema["properties"].contains("legacy"));
  HTMLPPT_CHECK(schema["$defs"].contains("target"));
  HTMLPPT_CHECK(schema["$defs"]["target"]["properties"].contains("html"));
  HTMLPPT_CHECK(schema["$defs"]["target"]["properties"].contains("edgeProfile"));
  HTMLPPT_CHECK(schema["$defs"]["target"]["properties"]["trigger"]["enum"].size() >= 2);
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/config_schema.h` does not exist.

- [ ] **Step 3: Implement the schema**

Create `native/src/core/config_schema.h`:

```cpp
#pragma once

#include <string_view>

namespace htmlppt {

std::string_view ConfigSchema();

} // namespace htmlppt
```

Create `native/src/core/config_schema.cpp`:

```cpp
#include "core/config_schema.h"

namespace htmlppt {

std::string_view ConfigSchema() {
  return R"json({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "htmlppt watchdog config",
  "type": "object",
  "required": ["version"],
  "properties": {
    "version": { "const": 1 },
    "activeProfile": { "type": "string", "minLength": 1 },
    "deck": { "type": "string" },
    "stateDir": { "type": "string" },
    "stage": { "$ref": "#/$defs/stage" },
    "defaults": { "$ref": "#/$defs/defaults" },
    "targets": { "type": "array", "items": { "$ref": "#/$defs/target" } },
    "profiles": {
      "type": "object",
      "additionalProperties": { "$ref": "#/$defs/profile" }
    },
    "legacy": { "$ref": "#/$defs/legacy" }
  },
  "$defs": {
    "cover": {
      "type": "object",
      "properties": {
        "fadeMs": { "type": "integer", "minimum": 1 },
        "holdMs": { "type": "integer", "minimum": 0 },
        "maxHoldMs": { "type": "integer", "minimum": 0 }
      },
      "additionalProperties": false
    },
    "edge": {
      "type": "object",
      "properties": {
        "maxLiveMs": { "type": "integer", "minimum": 1 },
        "prewarmProfile": { "type": "boolean" },
        "args": { "type": "array", "items": { "type": "string" } }
      },
      "additionalProperties": false
    },
    "display": {
      "type": "object",
      "properties": { "monitor": { "type": "string", "minLength": 1 } },
      "additionalProperties": false
    },
    "focus": {
      "type": "object",
      "properties": {
        "strategy": { "type": "string", "minLength": 1 },
        "require": { "type": "boolean" }
      },
      "additionalProperties": false
    },
    "assets": {
      "type": "object",
      "properties": {
        "mode": { "enum": ["file", "tree", "direct"] },
        "root": { "type": "string" },
        "include": { "type": "array", "items": { "type": "string" } },
        "exclude": { "type": "array", "items": { "type": "string" } },
        "maxBytes": { "type": "integer", "minimum": 0 }
      },
      "additionalProperties": false
    },
    "match": {
      "type": "object",
      "properties": {
        "slideTitle": { "type": "string", "minLength": 1 },
        "slideName": { "type": "string", "minLength": 1 },
        "mode": { "enum": ["exact", "prefix", "regex"] }
      },
      "additionalProperties": false,
      "oneOf": [
        { "required": ["slideTitle"] },
        { "required": ["slideName"] }
      ]
    },
    "target": {
      "type": "object",
      "properties": {
        "slide": { "type": "integer", "minimum": 1 },
        "match": { "$ref": "#/$defs/match" },
        "html": { "type": "string", "minLength": 1 },
        "label": { "type": "string" },
        "edgeProfile": { "type": "string" },
        "stageName": { "type": "string", "pattern": "^[ -~]*$" },
        "assets": { "$ref": "#/$defs/assets" },
        "htmlQuery": { "type": "string" },
        "trigger": { "enum": ["leave-forward", "arrive"] },
        "cover": { "$ref": "#/$defs/cover" },
        "edge": { "$ref": "#/$defs/edge" },
        "display": { "$ref": "#/$defs/display" },
        "focus": { "$ref": "#/$defs/focus" },
        "onFailure": { "enum": ["self-heal", "strict", "safe", "legacy"] },
        "maxConsecutiveTriggerFailures": { "type": "integer", "minimum": 0 },
        "enabled": { "type": "boolean" },
        "tags": { "type": "object", "additionalProperties": { "type": "string" } }
      },
      "required": ["html"],
      "oneOf": [
        { "required": ["slide"] },
        { "required": ["match"] }
      ],
      "additionalProperties": false
    },
    "defaults": {
      "type": "object",
      "properties": {
        "trigger": { "enum": ["leave-forward", "arrive"] },
        "cover": { "$ref": "#/$defs/cover" },
        "edge": { "$ref": "#/$defs/edge" },
        "display": { "$ref": "#/$defs/display" },
        "focus": { "$ref": "#/$defs/focus" },
        "scheduling": { "enum": ["adaptive", "aggressive", "balanced", "safe"] },
        "onFailure": { "enum": ["self-heal", "strict", "safe", "legacy"] },
        "maxConsecutiveTriggerFailures": { "type": "integer", "minimum": 0 }
      },
      "additionalProperties": false
    },
    "stage": {
      "type": "object",
      "properties": {
        "mode": { "enum": ["file", "tree", "direct"] },
        "root": { "type": "string" }
      },
      "additionalProperties": false
    },
    "profile": {
      "type": "object",
      "properties": {
        "deck": { "type": "string" },
        "stateDir": { "type": "string" },
        "stage": { "$ref": "#/$defs/stage" },
        "targets": { "type": "array", "items": { "$ref": "#/$defs/target" } }
      },
      "additionalProperties": false
    },
    "legacy": {
      "type": "object",
      "properties": {
        "deckName": { "type": "string" },
        "targetSlides": { "type": "array", "items": { "type": "integer", "minimum": 1 } },
        "srcNames": { "type": "array", "items": { "type": "string" } },
        "targetHtmls": { "type": "array", "items": { "type": "string" } },
        "targetProfiles": { "type": "array", "items": { "type": "string" } },
        "targetLabels": { "type": "array", "items": { "type": "string" } }
      },
      "additionalProperties": false
    }
  }
})json";
}

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/htmlppt_core_tests
```

Expected: the two schema tests pass; all previous tests pass; no warnings.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/config_schema.h native/src/core/config_schema.cpp native/tests/unit/config_schema_test.cpp
git commit -m "feat: add embedded config JSON schema"
```

---

## Self-Review

### Plan 2A spec coverage

| Spec requirement | Plan 2A coverage |
|---|---|
| `version`, `activeProfile`, `deck`, `stateDir`, `stage`, `defaults`, `targets`, `profiles`, `legacy` | Tasks 1-2 |
| Legacy six-array migration and equal-length validation | Task 2 |
| Profile selection; CLI `--profile`; precedence metadata (profile > top-level > legacy; CLI map highest) | Task 2 |
| Field-path validation and unknown-field rejection | Task 1 |
| Canonical `ConfigToJson` round-trip | Task 2 |
| Embedded JSON Schema | Task 3 |
| Deck-info, slide/title resolution, paths, auto names, CLI | Plan 2B |

No Plan 2A requirement is orphaned.

### Placeholder scan

- No `TBD`, `TODO`, `implement later`, or empty steps.
- Every code step contains the code to write.
- Every run step contains the exact command and expected result.
- The Task 1 parser intentionally handles top-level/defaults/targets; Task 2 replaces the same `LoadConfigFromString` to add profiles and legacy. This is an explicit incremental scope, not a placeholder.

### Type consistency

- `DefaultsConfig` is threaded into `ParseTarget`, `ParseTargets`, and `ParseProfile` so target-level `cover`/`edge`/`display`/`focus` inherit the actual defaults rather than struct defaults.
- `TargetConfig::source_priority` is the single precedence channel: legacy 0, top-level 1, profile 2, CLI map 3.
- `ConfigToJson` serializes `Config::active` and is declared in `config.h`.
- `ConfigSchema()` returns `std::string_view`; the test parses it with nlohmann/json.

---

## Execution Handoff

Plan 2A is ready. Continue with subagent-driven development: create a fresh worktree, commit this plan, then execute Tasks 1-3 with task-level reviews and a final whole-branch review. Plan 2B (deck-info, resolve, CLI) will be authored after Plan 2A lands.

---

## As-Built Amendments (2026-11-14)

Plan 2A was executed on branch `worktree-feat-cpp-watchdog-config-foundation`. The following approved amendments differ from the Task 1-3 code blocks above and are the source of truth for the implemented config foundation.

1. **Field-path convention**: field paths are root-relative (`targets[0].mystery`, `activeProfile`, `defaults.cover`), not `$`-prefixed. Only JSON parse errors and non-object roots use `$`. Task 1's implementation and the Task 2 code blocks were normalized accordingly.

2. **Task 1 follow-up fixes**:
   - `ParseEdge` and `ParseAssets` only overwrite `args` / `include` / `exclude` when the key is present, preserving defaults on partial overrides.
   - CLI `--map` targets inherit `defaults.trigger` and `defaults.on_failure`.
   - `assets.maxBytes` uses `ReadInt64`; values above `INT_MAX` are preserved.

3. **Task 2 follow-up fixes**:
   - Legacy targets inherit `defaults.trigger` and `defaults.on_failure`.
   - `ParseStage` and `ParseProfile` inherit stage fields one by one; profile `mode` and `root` are no longer lost.
   - `ConfigToJson` is a one-way effective-config snapshot: it expands profiles, drops legacy/raw profiles, omits `activeProfile`, does not serialize `source_priority`, and is not a profile-preserving save path. Reloading the snapshot produces no warnings.
   - Legacy `targetHtmls` values are validated as ASCII at `legacy.targetHtmls[i]`.

4. **Task 3 follow-up fix**: the root schema declares `additionalProperties: false`, matching the parser's rejection of unknown top-level fields.

5. **Final gate**: 47/47 unit cases, CTest 3/3, `npm test` 3/3, `-Werror` clean, working tree clean.

6. **Final review fixes (2026-11-14)**:
   - `display.monitor` accepts the spec's numeric form as well as strings. A JSON integer >= 1 is normalized to its decimal string (`2` becomes `"2"`); `primary`/`secondary` are preserved; any other string must parse completely with `std::from_chars` and be >= 1, so `"2abc"` is rejected. The embedded schema declares `display.monitor` as `oneOf` integer (minimum 1) or string (minLength 1).
   - Integer reads no longer narrow silently. `ReadInt64` rejects unsigned values above `INT64_MAX` with `integer is out of int64 range`; `ReadInt` builds on it and rejects values outside `INT_MIN..INT_MAX` with `integer is out of 32-bit range`; legacy `targetSlides` items use the same 32-bit check. `version` reports `required integer is missing` only when the key is absent, and a missing `html` produces one `required string is missing` error rather than also reporting emptiness.
   - Error results no longer carry a usable `Config`. `LoadConfigFromString` sets `result.config` only when no Error issue was recorded; warnings alone still yield a config, and `available_profiles` stays populated.
   - A single-profile config ignores `--profile` per spec 8.2: the sole profile is selected regardless of `activeProfile` or the CLI override, and `active_profile` is set to the sole profile name. Multiple profiles keep the CLI/`activeProfile` selection and the unknown-profile error.
   - A target-level `onFailure` present but empty is rejected with the enum error; only an absent key inherits `defaults.onFailure`.
   - A modern target `slide` below 1 is rejected with `must be >= 1`.

Deferred to Plan 2B/7: schema/parser key-parity automation, stageName strictness unification, `ConfigToJson` cannot preserve profile structure for the config UI, and source-priority-based target conflict resolution.

