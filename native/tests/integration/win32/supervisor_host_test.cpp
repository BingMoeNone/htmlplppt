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

#endif // _WIN32
