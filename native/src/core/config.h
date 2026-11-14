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
