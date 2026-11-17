#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/config.h"

namespace htmlppt {

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
