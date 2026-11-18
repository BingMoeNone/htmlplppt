#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <filesystem>
#include <string>

#include "app/watchdog_launcher.h"
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
  const std::wstring line = htmlppt::BuildSupervisorCommandLine(
      exe, config, state_dir, L"ui-1234");
  HTMLPPT_CHECK_EQ(
      line,
      std::wstring(
          L"\"C:\\Program Files\\htmlppt\\htmlppt-watchdog.exe\" "
          L"--supervisor --config \"C:\\Users\\me\\My Decks\\watcher.json\" "
          L"--state-dir \"C:\\Users\\me\\My Decks\\.htmlppt-run\" "
          L"--run-id \"ui-1234\""));
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

HTMLPPT_TEST_CASE(win32_build_command_lines_double_embedded_quotes) {
  // A Windows file name cannot contain a quote, but a run id can, and the
  // builders must still produce one argument: doubling is the documented
  // escape, so a quote reaches the child as a literal quote.
  const std::filesystem::path exe(L"C:\\bin\\watchdog.exe");
  const std::filesystem::path config(L"C:\\Decks\\we\"ird\\watcher.json");
  const std::filesystem::path state_dir(L"C:\\Decks\\we\"ird\\state");
  const std::wstring supervisor_line = htmlppt::BuildSupervisorCommandLine(
      exe, config, state_dir, L"ui 12\"34");
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
      std::filesystem::path(HTMLPPT_EXE_PATH), missing, state_dir, L"ui-test");
  DWORD exit_code = 0;
  std::wstring error;
  HTMLPPT_CHECK(htmlppt::RunAndWait(line, 30000, &exit_code, &error));
  // A config that cannot be loaded fails before any worker (or WPS) is
  // launched, so this case needs no deck and no WPS.
  HTMLPPT_CHECK_EQ(exit_code, static_cast<DWORD>(1));
}
#endif  // _WIN32
