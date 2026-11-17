#include "platform/win32/process_job.h"

#ifdef _WIN32

#include <utility>

namespace htmlppt {
namespace {

std::wstring QuoteArgument(const std::wstring& value) {
  const bool needs_quotes =
      value.empty() || value.find_first_of(L" \t\"") != std::wstring::npos;
  std::wstring result;
  if (needs_quotes) result.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  if (needs_quotes) {
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
  } else {
    result.append(backslashes, L'\\');
  }
  return result;
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
  const std::wstring command_line =
      BuildWindowsCommandLine(exe, args);
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
  if (!job.Assign(process_)) {
    TerminateProcess(process_, 1);
    Close();
    return false;
  }
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

std::wstring BuildWindowsCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments) {
  std::wstring line = QuoteArgument(executable.wstring());
  for (const auto& argument : arguments) {
    line.push_back(L' ');
    line += QuoteArgument(argument);
  }
  return line;
}

} // namespace htmlppt

#endif // _WIN32
