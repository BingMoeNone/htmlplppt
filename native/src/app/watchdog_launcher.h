#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace htmlppt {

// The command line that starts one supervisor for `config`, writing its run
// state under `state_dir`. Every path and the run id is quoted, and an embedded
// double quote is doubled, so a value with spaces (or a quote) still reaches the
// child as one argument. `allow_outside_folder` forwards the editor's
// --allow-outside-folder to the supervisor, so a config the editor validated
// with that permission resolves the same way during the run.
std::wstring BuildSupervisorCommandLine(const std::filesystem::path& exe,
                                        const std::filesystem::path& config,
                                        const std::filesystem::path& state_dir,
                                        const std::wstring& run_id,
                                        bool allow_outside_folder);

// The command line that asks the supervisor for `run_id` to stop. --stop acts
// through the run id's named stop event; --state-dir is passed for symmetry
// with the start line so both commands name the same run location.
std::wstring BuildStopCommandLine(const std::filesystem::path& exe,
                                  const std::filesystem::path& state_dir,
                                  const std::wstring& run_id);

// The state directory the editor gives an editor-launched run: a hidden
// ".htmlppt-run" sibling of the config file. Keeping it next to the config
// makes a run's plan and journal discoverable with the config without mixing
// them into the folder the user keeps watcher.json in. A relative config path
// is made absolute first, so the child sees the directory the editor resolved.
std::filesystem::path WatchdogStateDir(const std::filesystem::path& config_path);

// The editor's run id: "ui-<stamp>". The caller supplies the stamp (the app
// uses GetTickCount64) so the spelling is testable.
std::wstring MakeUiRunId(std::uint64_t stamp);

// The status line while an editor-launched run is live: the run id and the last
// state the run journalled (the same name --status prints). An empty `state` is
// the moment right after launch, before the run's black box has a record.
std::wstring WatchdogStatusText(const std::wstring& run_id,
                                const std::wstring& state);

#ifdef _WIN32
// True when a supervisor owns the single-instance mutex for `state_dir`. The
// supervisor holds it for its whole run and closes it as its last cleanup step,
// so a live supervisor -- of any run id -- is what closes the editor's Start
// gate. Read-only: the probe opens and closes the mutex without owning it.
bool IsSupervisorRunning(const std::filesystem::path& state_dir);

// Launches `cmdline` with CREATE_NO_WINDOW and no inherited handles, so a
// detached supervisor cannot keep the editor's console or handles alive. On
// success fills `*out`; the caller owns both hProcess and hThread.
bool LaunchDetached(const std::wstring& cmdline, PROCESS_INFORMATION* out,
                    std::wstring* error);

// Runs `cmdline` and waits at most `timeout_ms` for it to exit, then reports its
// exit code. A timeout returns false with an error and terminates the child, so
// a stuck helper cannot outlive the editor.
bool RunAndWait(const std::wstring& cmdline, DWORD timeout_ms,
                DWORD* exit_code, std::wstring* error);
#endif

} // namespace htmlppt
