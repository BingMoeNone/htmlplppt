#include "app/cli.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/config.h"
#include "core/config_issue.h"
#include "core/config_schema.h"
#include "core/deck_info.h"
#include "core/resolve.h"
#include "core/version.h"

namespace htmlppt {
namespace {

enum class Command {
  Version,
  Help,
  Validate,
  Resolve,
  PrintConfig,
  PrintSchema,
};

struct CliOptions {
  Command command = Command::Version;
  bool command_set = false;
  std::string config_path;
  std::string deck_info_path;
  CliConfigOverrides overrides;
};

struct ParseResult {
  bool ok = true;
  std::string error;
  CliOptions options;
};

void PrintUsage(std::ostream& out) {
  out << "usage: htmlppt-watchdog [options]\n"
      << "\n"
      << "commands:\n"
      << "  --version              print version (default with no arguments)\n"
      << "  --help, -h             print this help\n"
      << "  --validate             validate the config and resolved targets\n"
      << "  --resolve              print the resolved slide-to-HTML mapping\n"
      << "  --print-config         print the effective config as JSON\n"
      << "  --print-schema         print the embedded config JSON Schema\n"
      << "\n"
      << "options:\n"
      << "  --config PATH          watcher JSON config file\n"
      << "  --profile NAME         select a config profile\n"
      << "  --deck-info PATH       diagnostic JSON deck info (slides/titles)\n"
      << "  --stage-mode MODE      file, tree, or direct\n"
      << "  --state-dir PATH       override the state directory\n"
      << "  --scheduling MODE      adaptive, aggressive, balanced, or safe\n"
      << "  --map SLIDE=HTML       add or override a target (repeatable)\n"
      << "  --allow-outside-folder allow HTML paths outside the config folder\n";
}

bool TakeValue(int argc, char** argv, int* index, std::string_view option,
               std::string* value, std::string* error) {
  if (*index + 1 >= argc) {
    *error = "missing value for " + std::string(option);
    return false;
  }
  ++*index;
  *value = argv[*index];
  return true;
}

bool SetCommand(CliOptions* options, Command command, std::string* error) {
  if (options->command_set && options->command != command) {
    *error = "only one command may be specified";
    return false;
  }
  options->command = command;
  options->command_set = true;
  return true;
}

bool ParseMapValue(const std::string& value, CliOptions* options,
                   std::string* error) {
  const auto equals = value.find('=');
  if (equals == std::string::npos) {
    *error = "--map expects SLIDE=HTML";
    return false;
  }
  const std::string slide_text = value.substr(0, equals);
  const std::string html = value.substr(equals + 1);
  int slide = 0;
  const auto parsed =
      std::from_chars(slide_text.data(), slide_text.data() + slide_text.size(),
                      slide);
  if (slide_text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != slide_text.data() + slide_text.size()) {
    *error = "--map slide must be an integer: " + slide_text;
    return false;
  }
  if (slide < 1) {
    *error = "--map slide must be >= 1";
    return false;
  }
  if (html.empty()) {
    *error = "--map HTML must not be empty";
    return false;
  }
  if (!options->overrides.map_overrides.emplace(slide, html).second) {
    *error = "--map slide is specified more than once: " + slide_text;
    return false;
  }
  return true;
}

ParseResult ParseCli(int argc, char** argv) {
  ParseResult result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string value;
    if (argument == "--version") {
      if (!SetCommand(&result.options, Command::Version, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--help" || argument == "-h") {
      if (!SetCommand(&result.options, Command::Help, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--validate") {
      if (!SetCommand(&result.options, Command::Validate, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--resolve") {
      if (!SetCommand(&result.options, Command::Resolve, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--print-config") {
      if (!SetCommand(&result.options, Command::PrintConfig, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--print-schema") {
      if (!SetCommand(&result.options, Command::PrintSchema, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--allow-outside-folder") {
      result.options.overrides.allow_outside_folder = true;
    } else if (argument == "--config") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.config_path = value;
    } else if (argument == "--profile") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.profile = value;
    } else if (argument == "--deck-info") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.deck_info_path = value;
    } else if (argument == "--stage-mode") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.stage_mode = value;
    } else if (argument == "--state-dir") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.state_dir = value;
    } else if (argument == "--scheduling") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.scheduling = value;
    } else if (argument == "--map") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      if (!ParseMapValue(value, &result.options, &result.error)) {
        result.ok = false;
        return result;
      }
    } else {
      result.ok = false;
      result.error = "unknown option: " + std::string(argument);
      return result;
    }
  }

  if (!result.options.command_set) {
    if (argc == 1) {
      result.options.command = Command::Version;
      result.options.command_set = true;
      return result;
    }
    result.ok = false;
    result.error =
        "no command specified (use --validate, --resolve, --print-config, or "
        "--print-schema)";
    return result;
  }

  const Command command = result.options.command;
  const bool needs_config = command == Command::Validate ||
                            command == Command::Resolve ||
                            command == Command::PrintConfig;
  if (needs_config && result.options.config_path.empty()) {
    result.ok = false;
    result.error = "--config is required for this command";
    return result;
  }
  if (!result.options.deck_info_path.empty() && command != Command::Validate &&
      command != Command::Resolve) {
    result.ok = false;
    result.error = "--deck-info is only valid with --validate or --resolve";
  }
  return result;
}

void PrintIssues(const std::vector<ConfigIssue>& issues, std::ostream& out) {
  for (const auto& issue : issues) {
    out << (issue.severity == IssueSeverity::Error ? "error: " : "warning: ")
        << issue.path << ": " << issue.message << "\n";
  }
}

std::size_t CountWarnings(const std::vector<ConfigIssue>& issues) {
  std::size_t count = 0;
  for (const auto& issue : issues) {
    if (issue.severity == IssueSeverity::Warning) ++count;
  }
  return count;
}

struct LoadedConfig {
  std::optional<Config> config;
  std::vector<ConfigIssue> issues;
};

LoadedConfig LoadConfigForCommand(const CliOptions& options) {
  auto loaded = LoadConfigFile(options.config_path, options.overrides);
  LoadedConfig result;
  result.config = std::move(loaded.config);
  result.issues = std::move(loaded.issues);
  return result;
}

struct DeckInfoLoad {
  std::vector<ConfigIssue> issues;
  std::optional<DeckInfo> info;
};

DeckInfoLoad LoadDeckInfoForCommand(const CliOptions& options,
                                    const Config& config) {
  DeckInfoLoad result;
  if (options.deck_info_path.empty()) return result;
  JsonDeckInfoProvider provider(options.deck_info_path);
  auto loaded = provider.Load(ResolveDeckPath(config));
  result.issues = std::move(loaded.issues);
  result.info = std::move(loaded.info);
  return result;
}

int RunValidate(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value()) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  Config& config = *loaded.config;
  std::vector<ConfigIssue> issues = std::move(loaded.issues);

  auto deck_info = LoadDeckInfoForCommand(options, config);
  for (auto& issue : deck_info.issues) issues.push_back(std::move(issue));

  ResolveOptions resolve_options;
  resolve_options.allow_outside_folder =
      options.overrides.allow_outside_folder;
  const auto resolved = ResolveTargets(
      config, deck_info.info ? &*deck_info.info : nullptr, resolve_options);
  for (const auto& issue : resolved.issues) issues.push_back(issue);

  PrintIssues(issues, std::cerr);
  if (HasErrorIssue(issues)) {
    std::cerr << "FAILED: configuration is invalid\n";
    return 1;
  }
  std::cout << "OK: configuration is valid (" << resolved.targets.size()
            << " targets, " << CountWarnings(issues) << " warnings)\n";
  return 0;
}

int RunResolve(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value()) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  Config& config = *loaded.config;
  std::vector<ConfigIssue> issues = std::move(loaded.issues);

  auto deck_info = LoadDeckInfoForCommand(options, config);
  for (auto& issue : deck_info.issues) issues.push_back(std::move(issue));

  ResolveOptions resolve_options;
  resolve_options.allow_outside_folder =
      options.overrides.allow_outside_folder;
  const auto resolved = ResolveTargets(
      config, deck_info.info ? &*deck_info.info : nullptr, resolve_options);

  std::cout << ResolveTableToText(resolved);
  for (const auto& issue : resolved.issues) issues.push_back(issue);
  PrintIssues(issues, std::cerr);
  return HasErrorIssue(issues) ? 1 : 0;
}

int RunPrintConfig(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value() || HasErrorIssue(loaded.issues)) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  std::cout << ConfigToJson(*loaded.config) << '\n';
  return 0;
}

} // namespace

int RunCli(int argc, char** argv) {
  ParseResult parsed = ParseCli(argc, argv);
  if (!parsed.ok) {
    std::cerr << "error: " << parsed.error << "\n";
    PrintUsage(std::cerr);
    return 2;
  }
  switch (parsed.options.command) {
    case Command::Version:
      std::cout << "htmlppt-watchdog " << version() << "\n";
      return 0;
    case Command::Help:
      PrintUsage(std::cout);
      return 0;
    case Command::Validate:
      return RunValidate(parsed.options);
    case Command::Resolve:
      return RunResolve(parsed.options);
    case Command::PrintConfig:
      return RunPrintConfig(parsed.options);
    case Command::PrintSchema:
      std::cout << ConfigSchema() << "\n";
      return 0;
  }
  return 2;
}

} // namespace htmlppt
