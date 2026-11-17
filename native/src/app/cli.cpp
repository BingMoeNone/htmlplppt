#include "app/cli.h"

#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "app/config_ui.h"
#include "app/deck_info_loader.h"
#include "app/supervisor.h"
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
  Supervisor,
  Worker,
  Stop,
  Status,
  Diagnostics,
  ConfigUi,
  DumpHelper,
};

struct CliOptions {
  Command command = Command::Version;
  bool command_set = false;
  std::string config_path;
  std::string deck_info_path;
  std::string run_id;
  std::uint32_t dump_pid = 0;
  std::string dump_path;
  std::string diagnostics_out;
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
      << "  --supervisor           run the supervisor host (Windows only)\n"
      << "  --worker               run the worker host (Windows only)\n"
      << "  --stop                 stop a running supervisor (Windows only)\n"
      << "  --status               print the run black box (Windows only)\n"
      << "  --diagnostics          write a diagnostics package (needs --out PATH)\n"
      << "  --config-ui            open the native config editor (Windows only)\n"
      << "  --dump-helper PID PATH write a MiniDump for a process (Windows "
         "only)\n"
      << "\n"
      << "options:\n"
      << "  --config PATH          watcher JSON config file\n"
      << "  --profile NAME         select a config profile\n"
      << "  --deck-info PATH       diagnostic JSON deck info (slides/titles)\n"
      << "  --stage-mode MODE      file, tree, or direct\n"
      << "  --state-dir PATH       override the state directory\n"
      << "  --run-id ID            supervisor run identifier\n"
      << "  --scheduling MODE      adaptive, aggressive, balanced, or safe\n"
      << "  --map SLIDE=HTML       add a target (repeatable; override is deferred)\n"
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

bool IsOneOf(std::string_view value,
             std::initializer_list<std::string_view> allowed) {
  for (const auto candidate : allowed) {
    if (value == candidate) return true;
  }
  return false;
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
    } else if (argument == "--supervisor") {
      if (!SetCommand(&result.options, Command::Supervisor, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--worker") {
      if (!SetCommand(&result.options, Command::Worker, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--stop") {
      if (!SetCommand(&result.options, Command::Stop, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--status") {
      if (!SetCommand(&result.options, Command::Status, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--diagnostics") {
      if (!SetCommand(&result.options, Command::Diagnostics, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--config-ui") {
      if (!SetCommand(&result.options, Command::ConfigUi, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--dump-helper") {
      if (!SetCommand(&result.options, Command::DumpHelper, &result.error)) {
        result.ok = false;
        return result;
      }
      std::string pid_text;
      if (!TakeValue(argc, argv, &i, argument, &pid_text, &result.error)) {
        result.ok = false;
        return result;
      }
      std::uint32_t pid = 0;
      const auto parsed = std::from_chars(
          pid_text.data(), pid_text.data() + pid_text.size(), pid);
      if (pid_text.empty() || parsed.ec != std::errc{} ||
          parsed.ptr != pid_text.data() + pid_text.size() || pid < 1) {
        result.ok = false;
        result.error = "--dump-helper PID must be an integer >= 1";
        return result;
      }
      result.options.dump_pid = pid;
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.dump_path = value;
    } else if (argument == "--run-id") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.run_id = value;
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
      if (!IsOneOf(value, {"file", "tree", "direct"})) {
        result.ok = false;
        result.error = "--stage-mode must be file, tree, or direct";
        return result;
      }
      result.options.overrides.stage_mode = value;
    } else if (argument == "--state-dir") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.state_dir = value;
    } else if (argument == "--out") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.diagnostics_out = value;
    } else if (argument == "--scheduling") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      if (!IsOneOf(value, {"adaptive", "aggressive", "balanced", "safe"})) {
        result.ok = false;
        result.error =
            "--scheduling must be adaptive, aggressive, balanced, or safe";
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
                            command == Command::PrintConfig ||
                            command == Command::Supervisor;
  if (needs_config && result.options.config_path.empty()) {
    result.ok = false;
    result.error = command == Command::Supervisor
                       ? "--config is required for --supervisor"
                       : "--config is required for this command";
    return result;
  }
  if (!result.options.deck_info_path.empty() && command != Command::Validate &&
      command != Command::Resolve && command != Command::Supervisor &&
      command != Command::ConfigUi) {
    result.ok = false;
    result.error =
        "--deck-info is only valid with --validate, --resolve, --supervisor, or "
        "--config-ui";
    return result;
  }
  const bool host_command =
      command == Command::Supervisor || command == Command::Worker ||
      command == Command::Stop || command == Command::Status ||
      command == Command::Diagnostics;
  if (!result.options.run_id.empty() && !host_command) {
    result.ok = false;
    result.error =
        "--run-id is only valid with --supervisor, --worker, --stop, "
        "--status, or --diagnostics";
    return result;
  }
  if ((command == Command::Stop || command == Command::Status ||
       command == Command::Diagnostics) &&
      result.options.run_id.empty()) {
    result.ok = false;
    result.error = "--run-id is required for --stop, --status, or --diagnostics";
    return result;
  }
  if (command == Command::ConfigUi && result.options.config_path.empty()) {
    result.ok = false;
    result.error = "--config-ui requires --config PATH";
    return result;
  }
  if (command == Command::DumpHelper &&
      (result.options.dump_pid == 0 || result.options.dump_path.empty())) {
    result.ok = false;
    result.error = "--dump-helper requires PID and PATH";
    return result;
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

int RunValidate(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value()) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  Config& config = *loaded.config;
  std::vector<ConfigIssue> issues = std::move(loaded.issues);

  auto deck_info = LoadDeckInfo(options.deck_info_path, config);
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

  auto deck_info = LoadDeckInfo(options.deck_info_path, config);
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
  const std::string state_dir =
      parsed.options.overrides.state_dir.value_or("");
  SupervisorRunOptions run_options;
  run_options.state_dir = state_dir;
  run_options.run_id = parsed.options.run_id;
  run_options.config_path = parsed.options.config_path;
  run_options.deck_info_path = parsed.options.deck_info_path;
  run_options.overrides = parsed.options.overrides;
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
    case Command::Supervisor:
      return RunSupervisor(run_options);
    case Command::Worker:
      return RunWorker(run_options);
    case Command::Stop:
      return RunStop(parsed.options.run_id);
    case Command::Status:
      return RunStatus(parsed.options.run_id, state_dir);
    case Command::Diagnostics: {
      if (parsed.options.diagnostics_out.empty()) {
        std::cerr << "error: --diagnostics requires --out PATH\n";
        return 2;
      }
      const auto dir = state_dir.empty() ? std::filesystem::path{}
                                         : std::filesystem::path(state_dir);
      return RunDiagnostics(parsed.options.run_id, dir,
                            std::filesystem::path(parsed.options.diagnostics_out));
    }
    case Command::ConfigUi: {
      ConfigUiOptions config_ui_options;
      config_ui_options.config_path = parsed.options.config_path;
      config_ui_options.deck_info_path = parsed.options.deck_info_path;
      config_ui_options.allow_outside_folder =
          parsed.options.overrides.allow_outside_folder;
      return RunConfigUi(config_ui_options);
    }
    case Command::DumpHelper:
      return RunDumpHelper(parsed.options.dump_pid, parsed.options.dump_path);
  }
  return 2;
}

} // namespace htmlppt
