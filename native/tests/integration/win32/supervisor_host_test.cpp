#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <objbase.h>

#include <algorithm>
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
#include "platform/win32/process_query.h"
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

// True when the WPS Presentation COM class is registered. This is a registry
// lookup only, so probing never starts WPS (preflight-live.ps1 does the same).
bool WpsInstalled() {
  CLSID clsid{};
  return CLSIDFromProgID(L"Kwpp.Application", &clsid) == S_OK;
}

std::vector<std::uint32_t> WpsProcessIds() {
  std::vector<std::uint32_t> ids;
  // The Presentation host image name differs between WPS builds.
  for (const wchar_t* name : {L"wps.exe", L"wpp.exe", L"kwpp.exe"}) {
    for (const auto id : htmlppt::ProcessIdsByName(name)) {
      ids.push_back(id);
    }
  }
  return ids;
}

// Only the Presentation host matters for the "kill the show" scenario: WPS keeps
// a persistent home/CEF process named wps.exe alive, but the slideshow COM
// server is wpp.exe. A leftover wpp.exe is what makes COM reuse an instance.
std::vector<std::uint32_t> WpsPresentationProcessIds() {
  std::vector<std::uint32_t> ids;
  for (const wchar_t* name : {L"wpp.exe", L"kwpp.exe"}) {
    for (const auto id : htmlppt::ProcessIdsByName(name)) {
      ids.push_back(id);
    }
  }
  return ids;
}

// Terminates only the WPS processes this test started, so repeated runs do not
// accumulate automation instances and their open deck windows. WPS is started
// through COM outside the worker's job, so the job does not reach it; instances
// that already existed (the operator's) are never touched.
//
// This also means the "kill WPS and expect a clean exit" case needs a clean
// starting state: with an automation instance already running, COM reuses it,
// killing the newly spawned pids does not remove the server the worker talks
// to, and the scenario silently becomes a no-op. That case skips itself when a
// WPS process already exists.
class WpsTestProcesses {
 public:
  WpsTestProcesses() : before_(WpsProcessIds()) {}
  ~WpsTestProcesses() { TerminateNew(); }

  // Terminates every WPS process that appeared after this tracker was created,
  // leaving instances that already existed (the operator's) alone. Safe to call
  // more than once.
  void TerminateNew() {
    for (const auto id : WpsProcessIds()) {
      if (std::find(before_.begin(), before_.end(), id) != before_.end()) {
        continue;
      }
      HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, id);
      if (process == nullptr) continue;
      TerminateProcess(process, 1);
      CloseHandle(process);
    }
  }

  WpsTestProcesses(const WpsTestProcesses&) = delete;
  WpsTestProcesses& operator=(const WpsTestProcesses&) = delete;

 private:
  std::vector<std::uint32_t> before_;
};

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

// A1: the supervisor reconciles the journal a previous run left behind before
// it resolves the config or writes a plan. The deliberately missing config still
// fails the run with exit 1, but only after the reconcile record is journaled:
// a non-Clean reconcile decision must never block or mask startup.
HTMLPPT_TEST_CASE(win32_supervisor_reconciles_the_previous_journal) {
  const auto state_dir = TempStateDir();
  {
    std::ofstream journal(state_dir / L"journal.jsonl", std::ios::binary);
    journal << "{\"event\":\"start\",\"state\":\"Live\",\"run_id\":\"prev\","
               "\"sequence\":0,\"timestamp\":\"2026-01-01T00:00:00Z\","
               "\"slide\":1,\"detail\":\"worker started\"}\n";
  }
  const std::string run_id = RunId();
  const auto result = RunCli(
      {L"--supervisor", L"--config", (state_dir / L"missing.json").wstring(),
       L"--state-dir", state_dir.wstring(), L"--run-id",
       std::wstring(run_id.begin(), run_id.end())},
      10000);
  HTMLPPT_CHECK(result.completed);
  HTMLPPT_CHECK_EQ(result.exit_code, DWORD{1});
  const std::string journal = ReadTextFile(state_dir / L"journal.jsonl");
  // The previous record belongs to a foreign run that left a Live state behind,
  // so the engine's decision is Cleanup; the outcome name is journaled verbatim.
  HTMLPPT_CHECK(journal.find("reconcile: cleanup") != std::string::npos);
  const auto reconcile_at = journal.find("reconcile:");
  const auto plan_failed_at = journal.find("plan failed");
  HTMLPPT_CHECK(plan_failed_at != std::string::npos);
  HTMLPPT_CHECK(reconcile_at < plan_failed_at);
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_supervisor_starts_and_stops) {
  // This case runs the worker end to end, which opens the deck in real WPS.
  // Without the WPS Presentation COM class the worker cannot present, so the
  // case reports a skip instead of failing the suite on machines without WPS.
  if (!WpsInstalled()) {
    std::cout << "SKIP: WPS not installed\n";
    return;
  }
  WpsTestProcesses wps_processes;
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
  // The effective config (CLI overrides and profile baked in) is handed to the
  // worker next to the plan, so its BuildStateMachineConfig matches the plan.
  HTMLPPT_CHECK(std::filesystem::is_regular_file(state_dir /
                                                 L"worker-config.json"));
  HTMLPPT_CHECK(ReadTextFile(state_dir / L"journal.jsonl")
                    .find("worker plan written") != std::string::npos);
  // Legacy compatibility mirror: VBS-era tooling finds the run through
  // watcher.lock next to the config while it is alive.
  HTMLPPT_CHECK(std::filesystem::exists(state_dir / L"watcher.lock"));

  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  DWORD exit_code = 0;
  GetExitCodeProcess(supervisor.process(), &exit_code);
  HTMLPPT_CHECK_EQ(exit_code, DWORD{0});
  // On a clean stop the worker removes the flags it owns and flushes its
  // journal into watch-log.txt, which is what the VBS tooling reads.
  HTMLPPT_CHECK(!std::filesystem::exists(state_dir / L"watcher.lock"));
  const std::string watch_log = ReadTextFile(state_dir / L"watch-log.txt");
  HTMLPPT_CHECK(watch_log.find("native watchdog start") != std::string::npos);
  HTMLPPT_CHECK(watch_log.find("native watchdog stop") != std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

// I3: a worker that sends Cleanup and exits 0 has ended the run by itself; the
// supervisor must journal that and exit 0 rather than request a dump and
// restart the worker (which would reopen the deck and restart the show). Taking
// the worker's WPS presentation away drives the state machine into SafeMode,
// the production machine-driven end.
HTMLPPT_TEST_CASE(win32_supervisor_clean_worker_exit_is_terminal) {
  if (!WpsInstalled()) {
    std::cout << "SKIP: WPS not installed\n";
    return;
  }
  // A pre-existing Presentation automation instance is reused through COM, so
  // killing the pids this run spawns would not remove the server the worker
  // talks to. The scenario needs a fresh instance; skip instead of reporting a
  // false pass.
  if (!WpsPresentationProcessIds().empty()) {
    std::cout << "SKIP: a WPS presentation instance is already running\n";
    return;
  }
  WpsTestProcesses wps_processes;
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

  // Wait until the worker opened the deck and started the real show, so it owns
  // a WPS instance the test can take away.
  bool saw_show_start = false;
  for (int i = 0; i < 150; ++i) {
    auto blackbox = SharedBlackbox::Open(
        htmlppt::SupervisorBlackboxName(run_id));
    if (blackbox != nullptr) {
      for (const auto& record : blackbox->Snapshot()) {
        if (std::string(record.detail).find("start show") != std::string::npos) {
          saw_show_start = true;
          break;
        }
      }
    }
    if (saw_show_start) break;
    Sleep(100);
  }
  HTMLPPT_CHECK(saw_show_start);

  // Killing WPS makes every COM QueryShow() fail; the COM error window expires
  // after 2 s and the machine enters SafeMode, so the session sends Cleanup and
  // exits 0.
  wps_processes.TerminateNew();

  const bool exited = supervisor.WaitForExit(20000);
  if (!exited) {
    // Best-effort teardown so a failing case does not leak a running run.
    SetEvent(stop);
    supervisor.WaitForExit(5000);
    supervisor.Terminate();
  }
  CloseHandle(stop);
  HTMLPPT_CHECK(exited);
  DWORD exit_code = 0;
  GetExitCodeProcess(supervisor.process(), &exit_code);
  HTMLPPT_CHECK_EQ(exit_code, DWORD{0});
  const std::string journal = ReadTextFile(state_dir / L"journal.jsonl");
  HTMLPPT_CHECK(journal.find("clean exit") != std::string::npos);
  // The journal is JSONL: a restart is event "restart" with detail
  // "worker restarted", so match the detail text, not a display label.
  HTMLPPT_CHECK(journal.find("worker restarted") == std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_supervisor_restarts_crashed_worker) {
  WpsTestProcesses wps_processes;
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
  WpsTestProcesses wps_processes;
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
  WpsTestProcesses wps_processes;
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
  WpsTestProcesses wps_processes;
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

  // Minor 11: when the supervisor's baked config is present, --worker must say
  // it is preferring that file over the --config it was given.
  {
    std::ofstream baked(state_dir / L"worker-config.json", std::ios::binary);
    baked << ReadTextFile(inputs.config);
  }
  std::ostringstream override_captured;
  previous = std::cerr.rdbuf(override_captured.rdbuf());
  const int override_rc = htmlppt::RunWorker(options);
  std::cerr.rdbuf(previous);
  HTMLPPT_CHECK_EQ(override_rc, 1);
  HTMLPPT_CHECK(override_captured.str().find("instead of --config") !=
                std::string::npos);

  std::error_code fs_error;
  std::filesystem::remove_all(state_dir, fs_error);
}

#endif // _WIN32

HTMLPPT_TEST_CASE(win32_diagnostics_package_collects_the_run_files) {
  const auto state_dir = TempStateDir();
  {
    std::ofstream journal(state_dir / L"journal.jsonl", std::ios::binary);
    journal << "{\"detail\":\"supervisor start\",\"event\":\"start\"}\n";
    std::ofstream log(state_dir / L"watch-log.txt", std::ios::binary);
    log << "2026-01-01T00:00:00Z native watchdog start run=diag\n";
  }
  const auto out = state_dir / L"diag";
  HTMLPPT_CHECK_EQ(htmlppt::RunDiagnostics("diag-run", state_dir, out), 0);

  HTMLPPT_CHECK(std::filesystem::is_regular_file(out / L"manifest.txt"));
  HTMLPPT_CHECK(std::filesystem::is_regular_file(out / L"journal.jsonl"));
  HTMLPPT_CHECK(std::filesystem::is_regular_file(out / L"watch-log.txt"));
  const std::string manifest = ReadTextFile(out / L"manifest.txt");
  HTMLPPT_CHECK(manifest.find("run diag-run") != std::string::npos);
  HTMLPPT_CHECK(manifest.find("file journal.jsonl copied") != std::string::npos);
  // No supervisor is running for this id, so the package must say so instead of
  // silently omitting the black box.
  HTMLPPT_CHECK(manifest.find("blackbox unavailable") != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}