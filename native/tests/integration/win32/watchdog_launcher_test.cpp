#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/watchdog_launcher.h"
#include "core/compat_flags.h"
#include "core/single_instance.h"
#include "test_utils.h"

// The launcher's command-line builders are pure strings, so their exact output
// is the contract: the exe, the command, and every option in CLI order, with
// each path and the run id quoted.

HTMLPPT_TEST_CASE(win32_build_supervisor_command_line_quotes_every_argument) {
  const std::filesystem::path exe(
      L"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe");
  const std::filesystem::path config(
      L"C:\\Users\\me\\My Decks\\watcher.json");
  const std::filesystem::path state_dir(
      L"C:\\Users\\me\\My Decks\\.htmlppt-run");
  // Without the flag the line ends at the run id: the exact string also proves
  // the run gets no outside-folder permission it did not ask for.
  const std::wstring line = htmlppt::BuildSupervisorCommandLine(
      exe, config, state_dir, L"ui-1234", /*allow_outside_folder=*/false);
  HTMLPPT_CHECK_EQ(
      line,
      std::wstring(
          L"\"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe\" "
          L"--supervisor --config \"C:\\Users\\me\\My Decks\\watcher.json\" "
          L"--state-dir \"C:\\Users\\me\\My Decks\\.htmlppt-run\" "
          L"--run-id \"ui-1234\""));
}

HTMLPPT_TEST_CASE(
    win32_build_supervisor_command_line_forwards_allow_outside_folder) {
  // The editor validated the config with --allow-outside-folder, so the run must
  // resolve it with the same permission; the flag goes last, after the run id.
  const std::filesystem::path exe(
      L"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe");
  const std::filesystem::path config(
      L"C:\\Users\\me\\My Decks\\watcher.json");
  const std::filesystem::path state_dir(
      L"C:\\Users\\me\\My Decks\\.htmlppt-run");
  const std::wstring line = htmlppt::BuildSupervisorCommandLine(
      exe, config, state_dir, L"ui-1234", /*allow_outside_folder=*/true);
  HTMLPPT_CHECK_EQ(
      line,
      std::wstring(
          L"\"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe\" "
          L"--supervisor --config \"C:\\Users\\me\\My Decks\\watcher.json\" "
          L"--state-dir \"C:\\Users\\me\\My Decks\\.htmlppt-run\" "
          L"--run-id \"ui-1234\" --allow-outside-folder"));
}

HTMLPPT_TEST_CASE(win32_build_stop_command_line_quotes_every_argument) {
  const std::filesystem::path exe(L"C:\\htmlppt watchdog\\htmlppt-watchdog.exe");
  const std::filesystem::path state_dir(L"C:\\htmlppt watchdog\\state");
  const std::wstring line =
      htmlppt::BuildStopCommandLine(exe, state_dir, L"ui-1234");
  HTMLPPT_CHECK_EQ(
      line,
      std::wstring(
          L"\"C:\\htmlppt watchdog\\htmlppt-watchdog.exe\" --stop "
          L"--state-dir \"C:\\htmlppt watchdog\\state\" "
          L"--run-id \"ui-1234\""));
}

HTMLPPT_TEST_CASE(win32_build_reload_command_line_quotes_every_argument) {
  // The save path asks the running supervisor for a hot reload through the same
  // --reload command a user can run by hand; every path and the run id is
  // quoted so a deck folder with spaces still reaches the helper as one arg.
  const std::filesystem::path exe(
      L"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe");
  const std::filesystem::path state_dir(
      L"C:\\Users\\me\\My Decks\\.htmlppt-run");
  const std::wstring line =
      htmlppt::BuildReloadCommandLine(exe, state_dir, L"ui-1234");
  HTMLPPT_CHECK_EQ(
      line,
      std::wstring(
          L"\"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe\" --reload "
          L"--state-dir \"C:\\Users\\me\\My Decks\\.htmlppt-run\" "
          L"--run-id \"ui-1234\""));
}

HTMLPPT_TEST_CASE(win32_build_command_lines_double_embedded_quotes) {
  // A Windows file name cannot contain a quote, but a run id can, and the
  // builders must still produce one argument: doubling is the documented
  // escape, so a quote reaches the child as a literal quote.
  const std::filesystem::path exe(L"C:\\bin\\watchdog.exe");
  const std::filesystem::path config(L"C:\\Decks\\we\"ird\\watcher.json");
  const std::filesystem::path state_dir(L"C:\\Decks\\we\"ird\\state");
  const std::wstring supervisor_line = htmlppt::BuildSupervisorCommandLine(
      exe, config, state_dir, L"ui 12\"34", /*allow_outside_folder=*/false);
  HTMLPPT_CHECK_EQ(
      supervisor_line,
      std::wstring(
          L"\"C:\\bin\\watchdog.exe\" --supervisor "
          L"--config \"C:\\Decks\\we\"\"ird\\watcher.json\" "
          L"--state-dir \"C:\\Decks\\we\"\"ird\\state\" "
          L"--run-id \"ui 12\"\"34\""));

  const std::wstring stop_line =
      htmlppt::BuildStopCommandLine(exe, state_dir, L"ui 12\"34");
  HTMLPPT_CHECK_EQ(
      stop_line,
      std::wstring(L"\"C:\\bin\\watchdog.exe\" --stop "
                   L"--state-dir \"C:\\Decks\\we\"\"ird\\state\" "
                   L"--run-id \"ui 12\"\"34\""));
}

HTMLPPT_TEST_CASE(win32_watchdog_state_dir_sits_next_to_the_config) {
  HTMLPPT_CHECK_EQ(
      htmlppt::WatchdogStateDir(std::filesystem::path(L"C:\\decks\\watcher.json")),
      std::filesystem::path(L"C:\\decks\\.htmlppt-run"));
  // A relative config is made absolute first, so the child inherits the same
  // directory the editor resolved.
  const std::filesystem::path state = htmlppt::WatchdogStateDir(L"watcher.json");
  std::error_code error;
  const std::filesystem::path expected =
      std::filesystem::absolute(std::filesystem::path(L"watcher.json"), error)
          .parent_path() /
      L".htmlppt-run";
  HTMLPPT_CHECK(!error);
  HTMLPPT_CHECK_EQ(state, expected);
}

HTMLPPT_TEST_CASE(win32_ui_run_id_and_status_text_name_the_run) {
  HTMLPPT_CHECK_EQ(htmlppt::MakeUiRunId(0), std::wstring(L"ui-0"));
  HTMLPPT_CHECK_EQ(htmlppt::MakeUiRunId(1745832123456ULL),
                   std::wstring(L"ui-1745832123456"));
  HTMLPPT_CHECK_EQ(htmlppt::WatchdogStatusText(L"ui-1", L""),
                   std::wstring(L"watchdog started: ui-1"));
  HTMLPPT_CHECK_EQ(htmlppt::WatchdogStatusText(L"ui-1", L"Monitoring"),
                   std::wstring(L"watchdog ui-1: Monitoring"));
}

// Start's click-time decision is a pure function of the facts read at the
// click. The window's Start flag is cached view state and may be stale, so the
// two liveness probes are the authority: AlreadyRunning wins over every other
// outcome, so a second supervisor is never launched into a refusal. Either probe
// is enough: the state-dir mutex covers this editor's run, and the per-config
// watcher.lock covers a run whose state dir is elsewhere.
HTMLPPT_TEST_CASE(win32_decide_start_matrix) {
  using htmlppt::StartDecision;
  // The state-dir probe wins even when the model is dirty and the Save gate is
  // open or closed, and even when there is nothing to save.
  HTMLPPT_CHECK(htmlppt::DecideStart(true, false, true, true) ==
                StartDecision::AlreadyRunning);
  HTMLPPT_CHECK(htmlppt::DecideStart(true, false, false, true) ==
                StartDecision::AlreadyRunning);
  HTMLPPT_CHECK(htmlppt::DecideStart(true, false, true, false) ==
                StartDecision::AlreadyRunning);
  HTMLPPT_CHECK(htmlppt::DecideStart(true, false, false, false) ==
                StartDecision::AlreadyRunning);
  // The per-config lock probe wins just as hard, even without the mutex (a run
  // launched with a different --state-dir still owns this config).
  HTMLPPT_CHECK(htmlppt::DecideStart(false, true, true, true) ==
                StartDecision::AlreadyRunning);
  HTMLPPT_CHECK(htmlppt::DecideStart(false, true, false, true) ==
                StartDecision::AlreadyRunning);
  HTMLPPT_CHECK(htmlppt::DecideStart(false, true, false, false) ==
                StartDecision::AlreadyRunning);
  // Both probes clear: a clean model launches without saving.
  HTMLPPT_CHECK(htmlppt::DecideStart(false, false, false, false) ==
                StartDecision::Start);
  // A stale-open gate on a clean model is still a plain start.
  HTMLPPT_CHECK(htmlppt::DecideStart(false, false, true, false) ==
                StartDecision::Start);
  // Dirty and savable: the run must read the saved file.
  HTMLPPT_CHECK(htmlppt::DecideStart(false, false, true, true) ==
                StartDecision::NeedsSave);
  // Dirty but the Save gate is closed: the config cannot be persisted, so it
  // must not be launched.
  HTMLPPT_CHECK(htmlppt::DecideStart(false, false, false, true) ==
                StartDecision::BlockedConfig);
}

// The per-config gate reads the real compat lock, not the state dir: a worker
// writes watcher.lock next to the config it was launched with, so a run started
// from another editor (or with another --state-dir) is still detected.
HTMLPPT_TEST_CASE(win32_watchdog_live_for_config_reads_the_compat_lock) {
  htmlppt::test::TempDir directory;
  const std::filesystem::path config = directory.path() / L"watcher.json";
  const std::filesystem::path lock = directory.path() / L"watcher.lock";
  // No lock means no run.
  HTMLPPT_CHECK(!htmlppt::IsWatchdogLiveForConfig(config));

  // The real writer's format: a JSON lock whose pid is the owning process.
  htmlppt::CompatFlags live(directory.path(),
                            htmlppt::CompatIdentity{
                                "run-live", GetCurrentProcessId(), ""});
  HTMLPPT_CHECK(live.WriteLock("2026-11-19T00:00:00Z"));
  HTMLPPT_CHECK(std::filesystem::is_regular_file(lock));
  HTMLPPT_CHECK(htmlppt::IsWatchdogLiveForConfig(config));

  // Same format, but the pid belongs to a process that has already exited: a
  // leftover lock must not keep the Start gate closed.
  PROCESS_INFORMATION dead{};
  std::wstring launch_error;
  HTMLPPT_CHECK(htmlppt::LaunchDetached(L"cmd.exe /c exit 0", &dead,
                                        &launch_error));
  HTMLPPT_CHECK_EQ(WaitForSingleObject(dead.hProcess, 10000),
                   static_cast<DWORD>(WAIT_OBJECT_0));
  const DWORD dead_pid = dead.dwProcessId;
  CloseHandle(dead.hThread);
  CloseHandle(dead.hProcess);
  htmlppt::CompatFlags stale(
      directory.path(),
      htmlppt::CompatIdentity{"run-dead", dead_pid, ""});
  HTMLPPT_CHECK(stale.WriteLock("2026-11-19T00:00:00Z"));
  HTMLPPT_CHECK(!htmlppt::IsWatchdogLiveForConfig(config));

  // A lock with no readable owner is a VBS-era lock: trusted only while fresh.
  {
    std::ofstream out(lock, std::ios::binary | std::ios::trunc);
    out << "legacy-lock";
  }
  HTMLPPT_CHECK(htmlppt::IsWatchdogLiveForConfig(config));

  // Empty is not a live lock, and an old ownerless lock is stale even though it
  // is non-empty.
  {
    std::ofstream out(lock, std::ios::binary | std::ios::trunc);
  }
  HTMLPPT_CHECK(!htmlppt::IsWatchdogLiveForConfig(config));
  {
    std::ofstream out(lock, std::ios::binary | std::ios::trunc);
    out << "legacy-lock";
  }
  std::error_code time_error;
  std::filesystem::last_write_time(
      lock, std::filesystem::file_time_type::clock::now() -
                std::chrono::seconds(120),
      time_error);
  HTMLPPT_CHECK(!time_error);
  HTMLPPT_CHECK(!htmlppt::IsWatchdogLiveForConfig(config));
}

HTMLPPT_TEST_CASE(win32_watchdog_exit_text_reports_the_code) {
  // A refused launch must not look like a clean stop.
  HTMLPPT_CHECK_EQ(htmlppt::WatchdogExitText(0),
                   std::wstring(L"watchdog stopped"));
  HTMLPPT_CHECK_EQ(htmlppt::WatchdogExitText(1),
                   std::wstring(L"watchdog exited (code 1)"));
  HTMLPPT_CHECK_EQ(htmlppt::WatchdogExitText(0xC0000005u),
                   std::wstring(L"watchdog exited (code 3221225477)"));
}

HTMLPPT_TEST_CASE(win32_launch_detached_returns_ownable_handles) {
  PROCESS_INFORMATION child{};
  std::wstring error;
  HTMLPPT_CHECK(htmlppt::LaunchDetached(L"cmd.exe /c exit 7", &child, &error));
  HTMLPPT_CHECK(child.hProcess != nullptr);
  HTMLPPT_CHECK(child.hThread != nullptr);
  HTMLPPT_CHECK_EQ(WaitForSingleObject(child.hProcess, 10000),
                   static_cast<DWORD>(WAIT_OBJECT_0));
  DWORD exit_code = 0;
  HTMLPPT_CHECK(GetExitCodeProcess(child.hProcess, &exit_code) != 0);
  HTMLPPT_CHECK_EQ(exit_code, static_cast<DWORD>(7));
  // Both handles belong to the caller; closing them must succeed.
  HTMLPPT_CHECK(CloseHandle(child.hThread) != 0);
  HTMLPPT_CHECK(CloseHandle(child.hProcess) != 0);
}

HTMLPPT_TEST_CASE(win32_run_and_wait_returns_the_exit_code) {
  DWORD exit_code = 0;
  std::wstring error = L"unset";
  HTMLPPT_CHECK(
      htmlppt::RunAndWait(L"cmd.exe /c exit 3", 10000, &exit_code, &error));
  HTMLPPT_CHECK_EQ(exit_code, static_cast<DWORD>(3));
  HTMLPPT_CHECK(error.empty());
}

HTMLPPT_TEST_CASE(win32_run_and_wait_times_out_with_an_error) {
  DWORD exit_code = 0;
  std::wstring error;
  const ULONGLONG started = GetTickCount64();
  HTMLPPT_CHECK(!htmlppt::RunAndWait(L"cmd.exe /c ping -n 30 127.0.0.1 >nul",
                                     100, &exit_code, &error));
  HTMLPPT_CHECK(!error.empty());
  // The timeout bounds the wait: a 30 second ping must not stall the caller.
  HTMLPPT_CHECK(GetTickCount64() - started < 10000);
}

HTMLPPT_TEST_CASE(win32_detects_a_running_supervisor_by_its_mutex) {
  htmlppt::test::TempDir directory;
  const std::filesystem::path state_dir = directory.path() / L"state";
  std::error_code error;
  std::filesystem::create_directories(state_dir, error);
  HTMLPPT_CHECK(!error);
  HTMLPPT_CHECK(!htmlppt::IsSupervisorRunning(state_dir));

  // The supervisor holds this mutex for its whole run, so holding it is what
  // closes the editor's Start gate.
  const htmlppt::InstanceIdentity identity =
      htmlppt::MakeInstanceIdentity(state_dir, "supervisor");
  const std::wstring name(identity.mutex_name.begin(),
                          identity.mutex_name.end());
  HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
  HTMLPPT_CHECK(mutex != nullptr);
  HTMLPPT_CHECK(htmlppt::IsSupervisorRunning(state_dir));
  HTMLPPT_CHECK(CloseHandle(mutex) != 0);
  HTMLPPT_CHECK(!htmlppt::IsSupervisorRunning(state_dir));
}

HTMLPPT_TEST_CASE(win32_supervisor_with_a_missing_config_exits_one) {
  htmlppt::test::TempDir directory;
  // Missing on purpose: the supervisor must fail the plan step before it
  // launches a worker.
  const std::filesystem::path missing = directory.path() / L"missing.json";
  const std::filesystem::path state_dir = directory.path() / L"state";
  const std::wstring line = htmlppt::BuildSupervisorCommandLine(
      std::filesystem::path(HTMLPPT_EXE_PATH), missing, state_dir, L"ui-test",
      /*allow_outside_folder=*/false);
  DWORD exit_code = 0;
  std::wstring error;
  HTMLPPT_CHECK(htmlppt::RunAndWait(line, 30000, &exit_code, &error));
  // A config that cannot be loaded fails before any worker (or WPS) is
  // launched, so this case needs no deck and no WPS.
  HTMLPPT_CHECK_EQ(exit_code, static_cast<DWORD>(1));
}

// The thumbnail cleanup: a whole tree disappears, and a missing path is not an
// error (the editor also calls it before the first scan).
HTMLPPT_TEST_CASE(win32_remove_directory_recursively_removes_a_tree) {
  htmlppt::test::TempDir dir;
  const std::filesystem::path folder = dir.path() / L"thumbs";
  std::filesystem::create_directories(folder / L"nested");
  {
    std::ofstream(folder / L"slide-01.bmp") << "x";
    std::ofstream(folder / L"nested" / L"slide-02.bmp") << "x";
  }
  HTMLPPT_CHECK(std::filesystem::exists(folder));
  htmlppt::RemoveDirectoryRecursively(folder);
  HTMLPPT_CHECK(!std::filesystem::exists(folder));
  // Idempotent: removing a missing directory does nothing and does not throw.
  htmlppt::RemoveDirectoryRecursively(folder);
  HTMLPPT_CHECK(!std::filesystem::exists(folder));
}
#endif  // _WIN32
