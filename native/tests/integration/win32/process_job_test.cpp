#include "test_main.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

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

HTMLPPT_TEST_CASE(win32_process_resume_runs_child) {
  JobHandle job;
  ProcessHandle process;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(process.LaunchSuspended(
      CmdExe(), {L"/c", L"exit", L"0"}, job));
  HTMLPPT_CHECK(process.Resume());
  HTMLPPT_CHECK(process.WaitForExit(5000));
}

HTMLPPT_TEST_CASE(win32_job_kill_on_close) {
  ProcessHandle process;
  std::uint32_t pid = 0;
  {
    JobHandle job;
    LaunchChild(job, process);
    pid = process.pid();
    HTMLPPT_CHECK(job.valid());
    HTMLPPT_CHECK(IsProcessAlive(pid));
  }
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(pid));
}

HTMLPPT_TEST_CASE(win32_is_process_alive_reports_current_process) {
  HTMLPPT_CHECK(IsProcessAlive(GetCurrentProcessId()));
  HTMLPPT_CHECK(!IsProcessAlive(0xFFFFFFFFu));
}

using htmlppt::BuildWindowsCommandLine;

namespace {

std::vector<std::wstring> ParseCommandLine(const std::wstring& line) {
  int count = 0;
  LPWSTR* parsed = CommandLineToArgvW(line.c_str(), &count);
  if (parsed == nullptr) return {};
  std::vector<std::wstring> result(parsed, parsed + count);
  LocalFree(parsed);
  return result;
}

} // namespace

HTMLPPT_TEST_CASE(win32_command_line_round_trips_arguments) {
  const std::filesystem::path exe =
      L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe";
  const std::vector<std::wstring> arguments = {
      L"--app=file:///C:/demo/page.html",
      L"plain",
      L"\x6F22\x5B57",
  };
  const std::wstring line = BuildWindowsCommandLine(exe, arguments);
  const auto parsed = ParseCommandLine(line);
  HTMLPPT_CHECK_EQ(parsed.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(parsed[0], exe.wstring());
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    HTMLPPT_CHECK_EQ(parsed[i + 1], arguments[i]);
  }
}

HTMLPPT_TEST_CASE(win32_command_line_escapes_quotes_and_backslashes) {
  const std::filesystem::path exe = L"C:\\Tools\\arg echo.exe";
  const std::vector<std::wstring> arguments = {
      L"say \"hi\"",
      L"C:\\path with space\\",
      L"plain\\path",
  };
  const std::wstring line = BuildWindowsCommandLine(exe, arguments);
  const auto parsed = ParseCommandLine(line);
  HTMLPPT_CHECK_EQ(parsed.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(parsed[0], exe.wstring());
  HTMLPPT_CHECK_EQ(parsed[1], arguments[0]);
  HTMLPPT_CHECK_EQ(parsed[2], arguments[1]);
  HTMLPPT_CHECK_EQ(parsed[3], arguments[2]);
}

HTMLPPT_TEST_CASE(win32_command_line_preserves_empty_argument) {
  const std::wstring line =
      BuildWindowsCommandLine(L"C:\\helper.exe", {L"", L"x"});
  const auto parsed = ParseCommandLine(line);
  HTMLPPT_CHECK_EQ(parsed.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(parsed[1], std::wstring());
  HTMLPPT_CHECK_EQ(parsed[2], std::wstring(L"x"));
}

#endif // _WIN32
