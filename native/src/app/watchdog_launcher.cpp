#include "app/watchdog_launcher.h"

#include <initializer_list>
#include <system_error>
#include <utility>

#include "core/single_instance.h"

namespace htmlppt {
namespace {

// One argument, always quoted. A double quote is doubled: the CRT's argument
// parser reads a doubled quote inside a quoted string as one literal quote, so
// a value that contains a quote still arrives as a single argument.
std::wstring QuoteArgument(const std::wstring& value) {
  std::wstring quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back(L'"');
  for (const wchar_t character : value) {
    quoted.push_back(character);
    if (character == L'"') quoted.push_back(L'"');
  }
  quoted.push_back(L'"');
  return quoted;
}

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
      JoinArguments({QuoteArgument(exe.wstring()), L"--supervisor", L"--config",
                     QuoteArgument(config.wstring()), L"--state-dir",
                     QuoteArgument(state_dir.wstring()), L"--run-id",
                     QuoteArgument(run_id)});
  // The same permission the editor used for its own validation, in the CLI's
  // option order (a bare switch, so it is appended unquoted).
  if (allow_outside_folder) line += L" --allow-outside-folder";
  return line;
}

std::wstring BuildStopCommandLine(const std::filesystem::path& exe,
                                  const std::filesystem::path& state_dir,
                                  const std::wstring& run_id) {
  return JoinArguments({QuoteArgument(exe.wstring()), L"--stop", L"--state-dir",
                        QuoteArgument(state_dir.wstring()), L"--run-id",
                        QuoteArgument(run_id)});
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

StartDecision DecideStart(bool supervisor_running, bool save_enabled,
                          bool dirty) {
  if (supervisor_running) return StartDecision::AlreadyRunning;
  if (!dirty) return StartDecision::Start;
  // Dirty: the Save gate is the only thing that can persist the edit the run
  // would read. A closed gate is a blocked or multi-profile model.
  return save_enabled ? StartDecision::NeedsSave
                      : StartDecision::BlockedConfig;
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
