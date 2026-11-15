#include "platform/win32/process_job.h"

#ifdef _WIN32

#include <utility>

namespace htmlppt {
namespace {

std::wstring Quote(const std::wstring& value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
  return L"\"" + value + L"\"";
}

} // namespace

JobHandle::~JobHandle() {
  if (handle_ != nullptr) CloseHandle(handle_);
}

JobHandle::JobHandle(JobHandle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

JobHandle& JobHandle::operator=(JobHandle&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) CloseHandle(handle_);
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

bool JobHandle::Create() {
  if (handle_ != nullptr) return true;
  handle_ = CreateJobObjectW(nullptr, nullptr);
  if (handle_ == nullptr) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (SetInformationJobObject(handle_, JobObjectExtendedLimitInformation,
                              &limits, sizeof(limits)) == 0) {
    CloseHandle(handle_);
    handle_ = nullptr;
    return false;
  }
  return true;
}

bool JobHandle::Assign(HANDLE process) {
  return handle_ != nullptr && process != nullptr &&
         AssignProcessToJobObject(handle_, process) != 0;
}

bool JobHandle::Terminate() {
  return handle_ != nullptr && TerminateJobObject(handle_, 1) != 0;
}

ProcessHandle::~ProcessHandle() { Close(); }

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)),
      thread_(std::exchange(other.thread_, nullptr)),
      pid_(std::exchange(other.pid_, 0)) {}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
  if (this != &other) {
    Close();
    process_ = std::exchange(other.process_, nullptr);
    thread_ = std::exchange(other.thread_, nullptr);
    pid_ = std::exchange(other.pid_, 0);
  }
  return *this;
}

void ProcessHandle::Close() {
  if (thread_ != nullptr) {
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (process_ != nullptr) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  pid_ = 0;
}

bool ProcessHandle::LaunchSuspended(
    const std::filesystem::path& exe, const std::vector<std::wstring>& args,
    JobHandle& job) {
  if (!job.valid()) return false;
  Close();
  std::wstring command_line = Quote(exe.wstring());
  for (const auto& arg : args) {
    command_line += L" ";
    command_line += Quote(arg);
  }
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const BOOL created = CreateProcessW(
      exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
      nullptr, &startup, &information);
  if (created == FALSE) return false;
  process_ = information.hProcess;
  thread_ = information.hThread;
  pid_ = static_cast<std::uint32_t>(information.dwProcessId);
  return true;
}

bool ProcessHandle::Resume() {
  if (thread_ == nullptr) return false;
  const DWORD result = ResumeThread(thread_);
  CloseHandle(thread_);
  thread_ = nullptr;
  return result != static_cast<DWORD>(-1);
}

bool ProcessHandle::Terminate() {
  return process_ != nullptr && TerminateProcess(process_, 1) != 0;
}

bool ProcessHandle::WaitForExit(std::uint32_t timeout_ms) {
  if (process_ == nullptr) return false;
  return WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
}

bool IsProcessAlive(std::uint32_t pid) {
  if (pid == 0) return false;
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
  if (process == nullptr) return false;
  const DWORD result = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return result == WAIT_TIMEOUT;
}

} // namespace htmlppt

#endif // _WIN32
