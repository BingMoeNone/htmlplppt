#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/resolve.h"
#include "core/state_machine_types.h"

namespace htmlppt {

#ifdef _WIN32
// Fills each machine target's screen_width/height from its source target's
// display.monitor. Display metrics are a Windows fact, so this cannot live in
// the platform-neutral plan builder; `resolved` is the plan's index-aligned
// host-target vector, whose ordinal names the source config target. A source
// target without a display block leaves both fields empty (the worker keeps the
// host/default screen). A selection error is appended to `warnings` (used to
// warn and journal) while the primary fallback metrics are still applied, the
// same behaviour as the global display path.
void ResolveTargetScreens(RunPlan& plan, const Config& config,
                          const std::vector<ResolvedTarget>& resolved,
                          std::vector<std::string>* warnings = nullptr);
#endif

// Terminates every pid in the list and returns how many were terminated. Shared
// by startup reconciliation; the win32 suite drives it directly.
std::size_t TerminatePids(std::span<const std::uint32_t> pids);

// True when a command line contains the Edge profile path prefix, which includes
// the trailing separator after the run marker: "htmlppt-run-1\" matches run-1
// but not run-10.
bool CommandLineUsesProfile(const std::wstring& command_line,
                            const std::wstring& profile_prefix);

// Options for the host commands. Only the supervisor resolves the config: it
// carries the path plus the same CLI overrides --validate/--resolve use, and
// writes the resulting plan into the state directory for the worker.
// --worker keeps its existing options in this task.
struct SupervisorRunOptions {
  std::filesystem::path state_dir;
  std::string run_id;
  std::filesystem::path config_path;
  std::filesystem::path deck_info_path;
  CliConfigOverrides overrides;
};

inline std::wstring SupervisorPipeName(std::string_view run_id) {
  return L"\\\\.\\pipe\\htmlppt-" +
         std::wstring(run_id.begin(), run_id.end());
}

inline std::wstring SupervisorBlackboxName(std::string_view run_id) {
  return L"Local\\htmlppt-blackbox-" +
         std::wstring(run_id.begin(), run_id.end());
}

inline std::wstring SupervisorStopEventName(std::string_view run_id) {
  return L"Local\\htmlppt-stop-" +
         std::wstring(run_id.begin(), run_id.end());
}

int RunSupervisor(const SupervisorRunOptions& options);
int RunWorker(const SupervisorRunOptions& options);
int RunStop(std::string_view run_id);
int RunStatus(std::string_view run_id,
              const std::filesystem::path& state_dir);
// Writes a self-contained diagnostics package for a run: manifest, journal,
// legacy watch log, plan/config, black box snapshot and any crash dumps.
int RunDiagnostics(std::string_view run_id,
                   const std::filesystem::path& state_dir,
                   const std::filesystem::path& out_dir);
int RunDumpHelper(std::uint32_t pid, const std::filesystem::path& path);

} // namespace htmlppt
