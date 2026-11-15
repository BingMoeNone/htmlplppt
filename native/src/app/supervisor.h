#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace htmlppt {

struct SupervisorRunOptions {
  std::filesystem::path state_dir;
  std::string run_id;
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
int RunDumpHelper(std::uint32_t pid, const std::filesystem::path& path);

} // namespace htmlppt
