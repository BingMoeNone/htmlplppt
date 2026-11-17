#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/supervisor.h"
#include "app/worker_session.h"
#include "core/worker_plan.h"
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

std::uint32_t WaitForWorkerPid(const std::string& run_id,
                               std::uint32_t ignore_pid, int timeout_ms) {
  const auto deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    auto blackbox = SharedBlackbox::Open(
        htmlppt::SupervisorBlackboxName(run_id));
    if (blackbox != nullptr) {
      for (const auto& record : blackbox->Snapshot()) {
        if (record.pid != 0 && record.pid != ignore_pid) {
          return record.pid;
        }
      }
    }
    if (GetTickCount64() >= deadline) return 0;
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

// The supervisor resolves the config itself, so every launch needs a readable
// config plus the deck facts it would otherwise query from WPS. The fixture
// lives in the per-test state directory and is self-contained.
struct SupervisorInputs {
  std::filesystem::path config;
  std::filesystem::path deck_info;
};

SupervisorInputs WriteSupervisorInputs(
    const std::filesystem::path& state_dir) {
  const auto write = [&](const wchar_t* name, const std::string& text) {
    std::ofstream out(state_dir / name, std::ios::binary);
    out << text;
  };
  write(L"page1.html", "<html>1</html>");
  write(L"deck-info.json",
        R"({"slides":[{"index":1,"title":"Cover","name":"Slide1"}]})");
  // The worker opens the deck for real now, so the config points at the
  // fixture deck. It must be a deck WPS can open (a placeholder byte string
  // makes the automation Open call block on a corrupt-file prompt), and it must
  // outlive the test: WPS keeps the opened file while the state dir is deleted.
  const std::string deck =
      std::filesystem::path(HTMLPPT_DECK_PATH).generic_string();
  write(L"watcher.json",
        "{\n    \"version\": 1,\n    \"deck\": \"" + deck +
            "\",\n    \"targets\": [{ \"slide\": 1, \"html\": \"page1.html\", "
            "\"label\": \"Cover\" }]\n  }");
  return {state_dir / L"watcher.json", state_dir / L"deck-info.json"};
}

std::vector<std::wstring> SupervisorArgs(
    const std::filesystem::path& state_dir, const std::string& run_id,
    const SupervisorInputs& inputs) {
  return {L"--supervisor", L"--state-dir", state_dir.wstring(), L"--run-id",
          std::wstring(run_id.begin(), run_id.end()), L"--config",
          inputs.config.wstring(), L"--deck-info", inputs.deck_info.wstring()};
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

} // namespace

HTMLPPT_TEST_CASE(win32_supervisor_fails_before_launch_on_bad_config) {
  const auto state_dir = TempStateDir();
  const auto inputs = WriteSupervisorInputs(state_dir);
  // Resolving the config needs the HTML the target points at; without it the
  // supervisor must journal the failure and exit 1 instead of launching a
  // worker that cannot work.
  std::filesystem::remove(state_dir / L"page1.html");
  const auto result = RunCli(SupervisorArgs(state_dir, RunId(), inputs), 10000);
  HTMLPPT_CHECK(result.completed);
  HTMLPPT_CHECK_EQ(result.exit_code, DWORD{1});
  HTMLPPT_CHECK(ReadTextFile(state_dir / L"journal.jsonl")
                    .find("plan failed") != std::string::npos);
  HTMLPPT_CHECK(!std::filesystem::exists(state_dir / L"worker-plan.json"));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_supervisor_starts_and_stops) {
  const auto state_dir = TempStateDir();
  const auto inputs = WriteSupervisorInputs(state_dir);
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), SupervisorArgs(state_dir, run_id, inputs), job));
  HTMLPPT_CHECK(supervisor.Resume());

  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  bool saw_records = false;
  bool saw_show_start = false;
  for (int i = 0; i < 150; ++i) {
    auto blackbox = SharedBlackbox::Open(
        htmlppt::SupervisorBlackboxName(run_id));
    if (blackbox != nullptr) {
      if (blackbox->appended() > 0) saw_records = true;
      for (const auto& record : blackbox->Snapshot()) {
        if (std::string(record.detail).find("start show") != std::string::npos) {
          saw_show_start = true;
          break;
        }
      }
    }
    if (saw_records && saw_show_start) break;
    Sleep(100);
  }
  HTMLPPT_CHECK(saw_records);
  // The worker leaves Warming only on ShowStarted, so dropping StartShow would
  // leave it polling forever. The black box is the worker's journal here.
  HTMLPPT_CHECK(saw_show_start);

  // The supervisor writes the plan before it launches the worker, so a running
  // worker implies the file and the journal record exist.
  const auto plan_path = state_dir / L"worker-plan.json";
  HTMLPPT_CHECK(std::filesystem::is_regular_file(plan_path));
  const std::string plan = ReadTextFile(plan_path);
  HTMLPPT_CHECK(plan.find("\"runId\": \"" + run_id + "\"") != std::string::npos);
  HTMLPPT_CHECK(ReadTextFile(state_dir / L"journal.jsonl")
                    .find("worker plan written") != std::string::npos);

  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  DWORD exit_code = 0;
  GetExitCodeProcess(supervisor.process(), &exit_code);
  HTMLPPT_CHECK_EQ(exit_code, DWORD{0});
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_supervisor_restarts_crashed_worker) {
  const auto state_dir = TempStateDir();
  const auto inputs = WriteSupervisorInputs(state_dir);
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), SupervisorArgs(state_dir, run_id, inputs), job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  const std::uint32_t first_pid = WaitForWorkerPid(run_id, 0, 5000);
  HTMLPPT_CHECK(first_pid != 0);
  HANDLE worker = OpenProcess(PROCESS_TERMINATE, FALSE, first_pid);
  HTMLPPT_CHECK(worker != nullptr);
  HTMLPPT_CHECK(TerminateProcess(worker, 1));
  CloseHandle(worker);

  const std::uint32_t second_pid = WaitForWorkerPid(run_id, first_pid, 15000);
  HTMLPPT_CHECK(second_pid != 0);
  HTMLPPT_CHECK(second_pid != first_pid);

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
  const auto inputs = WriteSupervisorInputs(state_dir);
  const std::string first_id = RunId();
  const std::string second_id = RunId() + "-second";
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), SupervisorArgs(state_dir, first_id, inputs), job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(first_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  const auto duplicate = RunCli(SupervisorArgs(state_dir, second_id, inputs),
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
  const auto inputs = WriteSupervisorInputs(state_dir);
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), SupervisorArgs(state_dir, run_id, inputs), job));
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
  DWORD exit_code = 0;
  GetExitCodeProcess(supervisor.process(), &exit_code);
  HTMLPPT_CHECK_EQ(exit_code, DWORD{0});
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_status) {
  const auto state_dir = TempStateDir();
  const auto inputs = WriteSupervisorInputs(state_dir);
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), SupervisorArgs(state_dir, run_id, inputs), job));
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

HTMLPPT_TEST_CASE(worker_rejects_plan_with_foreign_run_id) {
  const auto state_dir = TempStateDir();
  const auto inputs = WriteSupervisorInputs(state_dir);

  htmlppt::WorkerPlan plan;
  plan.machine.run_id = "run-foreign";
  plan.machine.slide_count = 1;
  HTMLPPT_CHECK(htmlppt::WriteWorkerPlanFile(plan,
                                             state_dir / L"worker-plan.json",
                                             nullptr));

  std::string error;
  HTMLPPT_CHECK(!htmlppt::ValidateWorkerPlanRunId(plan, "run-local", &error));
  HTMLPPT_CHECK(error.find("run-foreign") != std::string::npos);
  HTMLPPT_CHECK(error.find("run-local") != std::string::npos);
  HTMLPPT_CHECK(htmlppt::ValidateWorkerPlanRunId(plan, "run-foreign", &error));
  HTMLPPT_CHECK(error.empty());

  // The same validation inside RunWorker must reject the stale plan with exit 1
  // and a message naming both ids, before any pipe or adapter is touched.
  htmlppt::SupervisorRunOptions options;
  options.state_dir = state_dir;
  options.run_id = "run-local";
  options.config_path = inputs.config;
  std::ostringstream captured;
  auto* previous = std::cerr.rdbuf(captured.rdbuf());
  const int rc = htmlppt::RunWorker(options);
  std::cerr.rdbuf(previous);
  HTMLPPT_CHECK_EQ(rc, 1);
  HTMLPPT_CHECK(captured.str().find("does not match") != std::string::npos);

  std::error_code fs_error;
  std::filesystem::remove_all(state_dir, fs_error);
}

#endif // _WIN32
