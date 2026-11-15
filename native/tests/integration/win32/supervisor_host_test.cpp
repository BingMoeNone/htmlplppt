#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/supervisor.h"
#include "platform/win32/process_job.h"
#include "platform/win32/shared_blackbox.h"

using htmlppt::JobHandle;
using htmlppt::ProcessHandle;
using htmlppt::SharedBlackbox;

namespace {

std::filesystem::path ExePath() {
  return std::filesystem::path(HTMLPPT_EXE_PATH);
}

std::filesystem::path TempStateDir() {
  const auto path = std::filesystem::temp_directory_path() /
                    (L"htmlppt-host-test-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(path);
  return path;
}

std::string RunId() {
  return "test-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64());
}

HANDLE WaitForStopEvent(const std::wstring& name, int timeout_ms) {
  const auto deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (event != nullptr) return event;
    if (GetTickCount64() >= deadline) return nullptr;
    Sleep(100);
  }
}

struct CliResult {
  bool completed = false;
  DWORD exit_code = 0;
};

CliResult RunCli(const std::vector<std::wstring>& args,
                 DWORD timeout_ms = 10000) {
  JobHandle job;
  ProcessHandle process;
  if (!job.Create() || !process.LaunchSuspended(ExePath(), args, job) ||
      !process.Resume()) {
    return {};
  }
  if (!process.WaitForExit(timeout_ms)) {
    process.Terminate();
    process.WaitForExit(2000);
    return {};
  }
  CliResult result;
  result.completed = true;
  GetExitCodeProcess(process.process(), &result.exit_code);
  return result;
}

} // namespace

HTMLPPT_TEST_CASE(win32_supervisor_starts_and_stops) {
  const auto state_dir = TempStateDir();
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());

  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  bool saw_records = false;
  for (int i = 0; i < 30; ++i) {
    auto blackbox = SharedBlackbox::Open(
        htmlppt::SupervisorBlackboxName(run_id));
    if (blackbox != nullptr && blackbox->appended() > 0) {
      saw_records = true;
      break;
    }
    Sleep(100);
  }
  HTMLPPT_CHECK(saw_records);

  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  DWORD exit_code = 0;
  GetExitCodeProcess(supervisor.process(), &exit_code);
  HTMLPPT_CHECK_EQ(exit_code, DWORD{0});
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_supervisor_single_instance) {
  const auto state_dir = TempStateDir();
  const std::string first_id = RunId();
  const std::string second_id = RunId() + "-second";
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(first_id.begin(), first_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(first_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  const auto duplicate = RunCli(
      {L"--supervisor", L"--state-dir", state_dir.wstring(), L"--run-id",
       std::wstring(second_id.begin(), second_id.end())},
      5000);
  HTMLPPT_CHECK(duplicate.completed);
  HTMLPPT_CHECK_EQ(duplicate.exit_code, DWORD{1});

  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_stop) {
  const auto state_dir = TempStateDir();
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);
  CloseHandle(stop);

  const auto stop_result = RunCli(
      {L"--stop", L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      5000);
  HTMLPPT_CHECK(stop_result.completed);
  HTMLPPT_CHECK_EQ(stop_result.exit_code, DWORD{0});
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_status) {
  const auto state_dir = TempStateDir();
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);
  Sleep(1500);
  const auto status = RunCli(
      {L"--status", L"--run-id", std::wstring(run_id.begin(), run_id.end()),
       L"--state-dir", state_dir.wstring()},
      5000);
  HTMLPPT_CHECK(status.completed);
  HTMLPPT_CHECK_EQ(status.exit_code, DWORD{0});
  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_dump_helper) {
  JobHandle child_job;
  ProcessHandle child;
  HTMLPPT_CHECK(child_job.Create());
  HTMLPPT_CHECK(child.LaunchSuspended(
      L"C:\\Windows\\System32\\cmd.exe",
      {L"/c", L"ping", L"-n", L"30", L"127.0.0.1"}, child_job));
  HTMLPPT_CHECK(child.Resume());
  const auto path = std::filesystem::temp_directory_path() /
                    (L"htmlppt-cli-dump-" + std::to_wstring(child.pid()) +
                     L".dmp");
  std::error_code error;
  std::filesystem::remove(path, error);
  const auto dump = RunCli(
      {L"--dump-helper", std::to_wstring(child.pid()), path.wstring()},
      5000);
  HTMLPPT_CHECK(dump.completed);
  HTMLPPT_CHECK_EQ(dump.exit_code, DWORD{0});
  HTMLPPT_CHECK(std::filesystem::exists(path));
  HTMLPPT_CHECK(std::filesystem::file_size(path, error) > 0);
  HTMLPPT_CHECK(!error);
  HTMLPPT_CHECK(child.Terminate());
  HTMLPPT_CHECK(child.WaitForExit(5000));
  std::filesystem::remove(path, error);
}

#endif // _WIN32
