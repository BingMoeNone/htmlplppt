#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/win32/process_job.h"

using htmlppt::IsProcessAlive;
using htmlppt::JobHandle;
using htmlppt::ProcessHandle;

namespace {

std::filesystem::path CmdExe() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"ComSpec", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return std::filesystem::path(buffer);
  return std::filesystem::path(L"C:\\Windows\\System32\\cmd.exe");
}

std::vector<std::wstring> SleepArgs() {
  return {L"/c", L"ping", L"-n", L"30", L"127.0.0.1"};
}

void LaunchChild(JobHandle& job, ProcessHandle& process) {
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(process.LaunchSuspended(CmdExe(), SleepArgs(), job));
  const bool assigned = job.Assign(process.process());
  if (!assigned) process.Terminate();
  HTMLPPT_CHECK(assigned);
  HTMLPPT_CHECK(process.Resume());
}

} // namespace

HTMLPPT_TEST_CASE(win32_job_create_assign_and_terminate) {
  JobHandle job;
  ProcessHandle process;
  LaunchChild(job, process);
  HTMLPPT_CHECK(process.pid() != 0);
  HTMLPPT_CHECK(IsProcessAlive(process.pid()));
  HTMLPPT_CHECK(job.Terminate());
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(process.pid()));
}

HTMLPPT_TEST_CASE(win32_process_resume_and_terminate) {
  JobHandle job;
  ProcessHandle process;
  LaunchChild(job, process);
  HTMLPPT_CHECK(process.Terminate());
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(process.pid()));
}

HTMLPPT_TEST_CASE(win32_job_kill_on_close) {
  ProcessHandle process;
  std::uint32_t pid = 0;
  {
    JobHandle job;
    LaunchChild(job, process);
    pid = process.pid();
    HTMLPPT_CHECK(job.valid());
  }
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(pid));
}

HTMLPPT_TEST_CASE(win32_is_process_alive_reports_current_process) {
  HTMLPPT_CHECK(IsProcessAlive(GetCurrentProcessId()));
  HTMLPPT_CHECK(!IsProcessAlive(0xFFFFFFFFu));
}

#endif // _WIN32
