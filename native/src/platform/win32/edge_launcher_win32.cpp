#include "platform/win32/edge_launcher_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <system_error>
#include <utility>
#include <vector>

#include "platform/win32/utf8_win32.h"

namespace htmlppt {

Win32EdgeProcess::Win32EdgeProcess(ProcessHandle process, JobHandle job)
    : process_(std::move(process)), job_(std::move(job)) {}

Win32EdgeProcess::~Win32EdgeProcess() = default;

std::uint32_t Win32EdgeProcess::pid() const { return process_.pid(); }

bool Win32EdgeProcess::alive() const {
  return process_.pid() != 0 && IsProcessAlive(process_.pid());
}

void Win32EdgeProcess::terminate() {
  if (job_.valid()) job_.Terminate();
  if (process_.pid() != 0) {
    process_.Terminate();
    process_.WaitForExit(5000);
  }
}

bool Win32EdgeProcess::release() {
  if (process_.pid() == 0) return true;
  if (job_.valid()) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = 0;
    if (SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation,
                                &limits,
                                static_cast<DWORD>(sizeof(limits))) == 0) {
      return false;
    }
    job_ = JobHandle{};
  }
  process_ = ProcessHandle{};
  return true;
}

std::unique_ptr<EdgeProcess> Win32EdgeLauncher::Launch(
    const EdgeLaunchSpec& spec) {
  if (spec.executable.empty()) return nullptr;
  JobHandle job;
  if (!job.Create()) return nullptr;
  std::vector<std::wstring> arguments;
  arguments.reserve(spec.arguments.size());
  for (const auto& argument : spec.arguments) {
    auto wide = Utf8ToWide(argument);
    if (!wide.has_value()) return nullptr;
    arguments.push_back(std::move(*wide));
  }
  ProcessHandle process;
  if (!process.LaunchSuspended(spec.executable, arguments, job)) {
    return nullptr;
  }
  if (!process.Resume()) return nullptr;
  return std::make_unique<Win32EdgeProcess>(std::move(process),
                                            std::move(job));
}

std::optional<std::filesystem::path> FindExecutable(
    std::wstring_view name,
    const std::vector<std::filesystem::path>& candidates) {
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    error.clear();
  }
  if (!name.empty()) {
    const std::wstring file_name(name);
    wchar_t found[MAX_PATH] = {};
    const DWORD length = SearchPathW(nullptr, file_name.c_str(), nullptr,
                                     MAX_PATH, found, nullptr);
    if (length > 0 && length < MAX_PATH) {
      return std::filesystem::path(std::wstring(found, length));
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> FindEdgeExecutable() {
  std::vector<std::filesystem::path> candidates;
  const wchar_t* variables[] = {L"ProgramFiles(x86)", L"ProgramFiles",
                                L"LOCALAPPDATA"};
  for (const wchar_t* variable : variables) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(variable, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) continue;
    candidates.push_back(std::filesystem::path(buffer) / L"Microsoft" /
                         L"Edge" / L"Application" / L"msedge.exe");
  }
  return FindExecutable(L"msedge.exe", candidates);
}

} // namespace htmlppt

#endif // _WIN32
