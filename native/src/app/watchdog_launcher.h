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

// The command line that asks the supervisor for `run_id` to hot reload: it
// writes the reload request the supervisor polls in `state_dir`. --reload needs
// no config -- the run already owns one -- but it does need the run's state
// directory, so both are named explicitly and quoted like every other argument.
std::wstring BuildReloadCommandLine(const std::filesystem::path& exe,
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

// The status line after a launched supervisor has exited. A clean exit reads
// "watchdog stopped"; any other code is reported, so a launch the supervisor
// refused cannot be mistaken for a clean stop.
std::wstring WatchdogExitText(std::uint32_t exit_code);

// What a Start click must do, from the facts read at click time. Both liveness
// flags are real probes, not the window's cached Start flag, which can be stale
// by the time the click arrives: `supervisor_running` is the state dir's
// single-instance mutex, and `watchdog_live` is the per-config watcher.lock a
// worker writes next to its config. AlreadyRunning wins over every other
// outcome, so a second supervisor is never launched into a refusal.
enum class StartDecision {
  Start,          // clean and runnable: launch now
  NeedsSave,      // dirty and savable: save, then launch
  BlockedConfig,  // dirty but the Save gate is closed (blocked, or profiles)
  AlreadyRunning, // a supervisor or a live worker already owns this config
};
StartDecision DecideStart(bool supervisor_running, bool watchdog_live,
                          bool save_enabled, bool dirty);

#ifdef _WIN32
// True when a supervisor owns the single-instance mutex for `state_dir`. The
// supervisor holds it for its whole run and closes it as its last cleanup step,
// so a live supervisor -- of any run id -- is what closes the editor's Start
// gate. Read-only: the probe opens and closes the mutex without owning it.
bool IsSupervisorRunning(const std::filesystem::path& state_dir);

// True when the config has a live watchdog behind its legacy watcher.lock. The
// worker writes that lock next to the config regardless of its state directory,
// so this finds a run the state-dir probe cannot see (launched by another editor
// or with another --state-dir). The lock is the JSON CompatFlags::WriteLock
// writes: a readable `pid` is checked with OpenProcess +
// GetExitCodeProcess == STILL_ACTIVE; a lock with no readable owner (VBS-era or
// truncated) counts as live only while non-empty and younger than 60 s, which is
// long enough for the worker's 5 s refresh to cover a slow start.
bool IsWatchdogLiveForConfig(const std::filesystem::path& config_path);

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

// Removes dir and everything under it. Missing paths are not an error; a
// locked entry is reported through the returned error code only.
void RemoveDirectoryRecursively(const std::filesystem::path& dir);

} // namespace htmlppt
