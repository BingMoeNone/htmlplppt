#include "app/watchdog_launcher.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/command_line.h"
#include "core/compat_flags.h"
#include "core/single_instance.h"
#include "core/utf8_path.h"

namespace htmlppt {

void RemoveDirectoryRecursively(const std::filesystem::path& dir) {
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}
namespace {

// One argument, always quoted, with the CRT's backslash/quote rules applied by
// QuoteWindowsArgument: a trailing backslash must not escape the closing quote.

std::wstring JoinArguments(std::initializer_list<std::wstring> arguments) {
  std::wstring line;
  for (const std::wstring& argument : arguments) {
    if (!line.empty()) line.push_back(L' ');
    line += argument;
  }
  return line;
}

} // namespace

std::wstring BuildSupervisorCommandLine(const std::filesystem::path& exe,
                                        const std::filesystem::path& config,
                                        const std::filesystem::path& state_dir,
                                        const std::wstring& run_id,
                                        bool allow_outside_folder) {
  std::wstring line =
      JoinArguments({QuoteWindowsArgument(exe.wstring()), L"--supervisor", L"--config",
                     QuoteWindowsArgument(config.wstring()), L"--state-dir",
                     QuoteWindowsArgument(state_dir.wstring()), L"--run-id",
                     QuoteWindowsArgument(run_id)});
  // The same permission the editor used for its own validation, in the CLI's
  // option order (a bare switch, so it is appended unquoted).
  if (allow_outside_folder) line += L" --allow-outside-folder";
  return line;
}

std::wstring BuildStopCommandLine(const std::filesystem::path& exe,
                                  const std::filesystem::path& state_dir,
                                  const std::wstring& run_id) {
  return JoinArguments({QuoteWindowsArgument(exe.wstring()), L"--stop", L"--state-dir",
                        QuoteWindowsArgument(state_dir.wstring()), L"--run-id",
                        QuoteWindowsArgument(run_id)});
}

std::wstring BuildReloadCommandLine(const std::filesystem::path& exe,
                                    const std::filesystem::path& state_dir,
                                    const std::wstring& run_id) {
  return JoinArguments({QuoteWindowsArgument(exe.wstring()), L"--reload",
                        L"--state-dir", QuoteWindowsArgument(state_dir.wstring()),
                        L"--run-id", QuoteWindowsArgument(run_id)});
}

std::filesystem::path WatchdogStateDir(
    const std::filesystem::path& config_path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(config_path, error);
  if (error) absolute = config_path;
  return absolute.parent_path() / L".htmlppt-run";
}

std::wstring MakeUiRunId(std::uint64_t stamp) {
  return L"ui-" + std::to_wstring(stamp);
}

std::wstring WatchdogStatusText(const std::wstring& run_id,
                                const std::wstring& state) {
  if (state.empty()) return L"watchdog started: " + run_id;
  return L"watchdog " + run_id + L": " + state;
}

std::wstring WatchdogExitText(std::uint32_t exit_code) {
  if (exit_code == 0) return L"watchdog stopped";
  return L"watchdog exited (code " + std::to_wstring(exit_code) + L")";
}

StartDecision DecideStart(bool supervisor_running, bool watchdog_live,
                          bool save_enabled, bool dirty) {
  if (supervisor_running || watchdog_live) return StartDecision::AlreadyRunning;
  if (!dirty) return StartDecision::Start;
  // Dirty: the Save gate is the only thing that can persist the edit the run
  // would read. A closed gate is a blocked or multi-profile model.
  return save_enabled ? StartDecision::NeedsSave
                      : StartDecision::BlockedConfig;
}

bool CanScanDeck(bool watchdog_running, bool supervisor_running,
                 bool watchdog_live) {
  return !watchdog_running && !supervisor_running && !watchdog_live;
}

#ifdef _WIN32
namespace {

// The readable form of a Win32 error, with the trailing newline trimmed so it
// can sit at the end of a one-line status message.
std::wstring Win32ErrorText(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::wstring message;
  if (length > 0 && buffer != nullptr) {
    message.assign(buffer, length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
      message.pop_back();
    }
  }
  if (buffer != nullptr) LocalFree(buffer);
  if (message.empty()) message = L"Windows error " + std::to_wstring(code);
  return message;
}

} // namespace

bool IsSupervisorRunning(const std::filesystem::path& state_dir) {
  const InstanceIdentity identity =
      MakeInstanceIdentity(state_dir, "supervisor");
  const std::wstring name(identity.mutex_name.begin(),
                          identity.mutex_name.end());
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
  if (mutex == nullptr) return false;
  CloseHandle(mutex);
  return true;
}

bool IsWatchdogLiveForConfig(const std::filesystem::path& config_path) {
  std::error_code cwd_error;
  // A bare `--config watcher.json` has no parent; resolve it against the
  // current directory exactly like the worker's compat mirror does, so the
  // editor's Start gate can see a run started by another process.
  const std::filesystem::path dir =
      ConfigDirFor(config_path, std::filesystem::current_path(cwd_error));
  if (dir.empty()) return false;
  const std::filesystem::path lock = dir / std::string(kLockFile);
  std::error_code error;
  if (!std::filesystem::is_regular_file(lock, error)) return false;

  std::ifstream input(lock, std::ios::binary);
  if (!input) return false;
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  if (text.empty()) return false;

  // Every liveness proof is bounded by the lock's age: the worker refreshes the
  // lock every heartbeat (~5 s), so a lock that has not been written for minutes
  // belongs to a run that is gone. Without this a recycled pid could keep the
  // Start gate closed forever.
  const auto written = std::filesystem::last_write_time(lock, error);
  if (error) return false;
  const auto age = std::filesystem::file_time_type::clock::now() - written;

  // The lock payload is the JSON CompatFlags::WriteLock writes. Its pid is the
  // worker that actually owns the run, so a live pid and a fresh lock are a live
  // watchdog. 120 s is twenty missed 5 s refreshes: long enough for a slow start,
  // short enough that a reused pid cannot wedge the gate.
  const auto parsed = nlohmann::json::parse(text, nullptr, false);
  if (!parsed.is_discarded() && parsed.is_object()) {
    const auto pid = parsed.find("pid");
    if (pid != parsed.end() && pid->is_number_integer()) {
      const std::int64_t raw = pid->get<std::int64_t>();
      if (raw > 0 && raw <= static_cast<std::int64_t>(UINT32_MAX)) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(raw));
        if (process == nullptr) {
          // Only access denied is ambiguous: a live supervisor owned by another
          // user/session rejects OpenProcess, so fall back to the lock age to
          // keep the gate closed. An invalid pid means the process is gone.
          if (GetLastError() == ERROR_ACCESS_DENIED) {
            return age < std::chrono::seconds(120);
          }
          return false;
        }
        DWORD exit_code = 0;
        const bool alive = GetExitCodeProcess(process, &exit_code) != FALSE &&
                           exit_code == STILL_ACTIVE;
        CloseHandle(process);
        return alive && age < std::chrono::seconds(120);
      }
    }
  }

  // No readable owner: a VBS-era watcher.lock. Trust it only while fresh. 60 s
  // leaves room for a slow start without trusting a crashed run's lock.
  return age < std::chrono::seconds(60);
}

bool LaunchDetached(const std::wstring& cmdline, PROCESS_INFORMATION* out,
                    std::wstring* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = L"no PROCESS_INFORMATION to fill";
    return false;
  }
  *out = PROCESS_INFORMATION{};
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  // CreateProcessW may write into the command-line buffer, so hand it a mutable
  // copy rather than the caller's string literal.
  std::wstring mutable_cmdline = cmdline;
  if (CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, out) ==
      FALSE) {
    if (error != nullptr) {
      *error = L"cannot start the process: " + Win32ErrorText(GetLastError());
    }
    return false;
  }
  return true;
}

bool RunAndWait(const std::wstring& cmdline, DWORD timeout_ms,
                DWORD* exit_code, std::wstring* error) {
  if (exit_code == nullptr) {
    if (error != nullptr) *error = L"no exit code to fill";
    return false;
  }
  PROCESS_INFORMATION child{};
  if (!LaunchDetached(cmdline, &child, error)) return false;
  CloseHandle(child.hThread);

  bool succeeded = false;
  const DWORD wait = WaitForSingleObject(child.hProcess, timeout_ms);
  if (wait == WAIT_OBJECT_0) {
    if (GetExitCodeProcess(child.hProcess, exit_code) != FALSE) {
      succeeded = true;
    } else if (error != nullptr) {
      *error = L"cannot read the exit code: " + Win32ErrorText(GetLastError());
    }
  } else if (wait == WAIT_TIMEOUT) {
    // A helper that outlives the wait must not outlive the editor: kill it and
    // wait for the termination so the handle can be closed cleanly.
    TerminateProcess(child.hProcess, 1);
    WaitForSingleObject(child.hProcess, 5000);
    if (error != nullptr) {
      *error = L"command timed out after " + std::to_wstring(timeout_ms) +
               L" ms: " + cmdline;
    }
  } else if (error != nullptr) {
    *error = L"cannot wait for the process: " + Win32ErrorText(GetLastError());
  }
  CloseHandle(child.hProcess);
  if (succeeded && error != nullptr) error->clear();
  return succeeded;
}
#endif // _WIN32

} // namespace htmlppt
