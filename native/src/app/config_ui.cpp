#include "app/config_ui.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#include "core/config.h"
#include "core/config_editor.h"
#include "core/deck_info.h"
#include "core/resolve.h"
#include "core/single_instance.h"
#include "core/types.h"
#include "app/deck_info_loader.h"
#include "app/supervisor.h"
#include "app/watchdog_launcher.h"
#include "platform/win32/config_file_watcher.h"
#include "platform/win32/config_window.h"
#include "platform/win32/shared_blackbox.h"

namespace htmlppt {
namespace {

// The editor serializes the effective profile only, so a profile-based config
// cannot be saved without dropping the other profiles. Read-only is the safe v1
// behaviour; the user edits the profile blocks in the JSON itself.
constexpr wchar_t kMultiProfileSaveDisabled[] =
    L"multi-profile config: save is disabled (edit profiles in the JSON)";

// The click-time probe found a supervisor holding the run's state directory --
// this editor's own run is handled separately, so this is another editor's (or
// a manually launched) run.
constexpr wchar_t kSupervisorAlreadyRunning[] =
    L"another watchdog is already running for this state dir";

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                       static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), size);
  return wide;
}

// The editor's own executable: the same binary runs as supervisor, so a run is
// launched by re-entering it with --supervisor.
std::filesystem::path CurrentExecutable() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(static_cast<std::size_t>(length));
      return std::filesystem::path(buffer);
    }
    // Truncated: the path is longer than the buffer, so grow and ask again.
    buffer.resize(buffer.size() * 2);
  }
}

// The state the run last journalled into its shared black box, spelled the way
// --status spells it. Empty before the supervisor has written a record (or when
// the mapping does not exist yet).
std::wstring ReadRunState(const std::wstring& run_id) {
  if (run_id.empty()) return {};
  const std::string narrow(run_id.begin(), run_id.end());
  auto blackbox = SharedBlackbox::Open(SupervisorBlackboxName(narrow));
  if (blackbox == nullptr) return {};
  const std::vector<BlackboxRecord> snapshot = blackbox->Snapshot();
  if (snapshot.empty()) return {};
  return Widen(StateName(static_cast<State>(snapshot.back().state)));
}

} // namespace

int RunConfigUi(const ConfigUiOptions& ui_options) {
  const std::filesystem::path& config_path = ui_options.config_path;
  // A missing or broken config is not fatal for the editor: the window opens
  // with an empty model and the loader's first issue in the status line, so the
  // user sees why instead of losing the window. The deck comes from the config
  // as a whole, so the window cannot repair the file; editing stays blocked
  // until the file is fixed outside it.
  const ConfigLoadResult loaded = LoadConfigFile(config_path);
  ConfigEditorModel model(loaded.config.has_value() ? *loaded.config : Config{});

  ResolveOptions resolve_options;
  resolve_options.allow_outside_folder = ui_options.allow_outside_folder;

  // Deck info reaches the editor the same way it reaches --validate: the JSON
  // provider when --deck-info is given, WPS otherwise. A config that failed to
  // load has no deck path to resolve, so no provider is asked.
  DeckInfoLoad deck;
  if (loaded.config.has_value()) {
    deck = LoadDeckInfo(ui_options.deck_info_path, *loaded.config);
  }
  const DeckInfo* deck_info = deck.info.has_value() ? &*deck.info : nullptr;
  const std::vector<DisplayRow> rows =
      model.DisplayRows(deck_info, resolve_options);

  // Declared before the window so it outlives the handlers stored in it.
  std::optional<std::size_t> selected_index;

  ConfigWindow window;
  ConfigWindowOptions options;
  options.title = L"htmlppt config - " + config_path.wstring();
  if (!window.Create(options)) {
    std::cerr << "error: cannot create the config window\n";
    return 1;
  }

  // The editor's own exe and the state directory an editor-launched run uses:
  // both are fixed for the life of the window. The run also gets the same
  // outside-folder permission the editor validated with, so the two cannot
  // disagree about a target's HTML path. The process handle below is the
  // one-run-at-a-time liveness source, so it is declared before the handlers
  // that launch, stop, and poll the run.
  const std::filesystem::path exe = CurrentExecutable();
  const std::filesystem::path state_dir = WatchdogStateDir(config_path);
  const bool allow_outside_folder = ui_options.allow_outside_folder;
  HANDLE watchdog_child = nullptr;
  bool watchdog_running = false;
  std::wstring run_id;
  std::wstring last_state;

  // Recomputes the rows, the Save gate, the Start gate, and the dirty flag after
  // every action, and returns the first blocking message: a target row's error
  // when there is one, otherwise the config-wide resolver error that has no row
  // of its own (a missing or invalid deck). Empty means the config is clean.
  const auto refresh = [&window, &model, deck_info, resolve_options,
                        &watchdog_running,
                        &state_dir]() -> std::wstring {
    const std::vector<DisplayRow> rows =
        model.DisplayRows(deck_info, resolve_options);
    window.SetRows(rows);
    const bool blocked = model.HasBlockingError(deck_info, resolve_options);
    // A profile-based config is never saveable: ConfigToJson writes only the
    // effective profile, so a save would silently drop the other profiles.
    const bool profiles_blocked = model.HasProfiles();
    window.SetSaveEnabled(model.dirty() && !blocked && !profiles_blocked);
    // Start needs a runnable config and a free state dir. A supervisor already
    // holding the state-dir mutex owns the run -- even when this editor did not
    // launch it -- so a second one must not be started (spec 9.6).
    window.SetStartEnabled(!blocked && !profiles_blocked && !watchdog_running &&
                           !IsSupervisorRunning(state_dir));
    window.SetDirty(model.dirty());
    if (profiles_blocked) return kMultiProfileSaveDisabled;
    if (!blocked) return {};
    for (const auto& row : rows) {
      if (row.state == "error") return Widen(row.issue);
    }
    for (const auto& issue : model.Validate(deck_info, resolve_options)) {
      if (issue.severity == IssueSeverity::Error) return Widen(issue.message);
    }
    return L"configuration is invalid";
  };

  // The view reports the selection as a model index, never as a list position.
  window.SetSelectionHandler([&selected_index](int model_index) {
    if (model_index < 0) {
      selected_index.reset();
    } else {
      selected_index = static_cast<std::size_t>(model_index);
    }
  });

  window.SetEditHandler(
      [&window, &model, &selected_index,
       refresh](const DetailFields& fields, std::wstring* error) {
        TargetEdit edit;
        std::wstring parse_error;
        if (!TargetEditFromDetailFields(fields, &edit, &parse_error)) {
          if (error != nullptr) *error = parse_error;
          return false;
        }
        if (!selected_index.has_value()) {
          if (error != nullptr) *error = L"no row selected";
          return false;
        }
        std::string model_error;
        if (!model.ApplyEdit(*selected_index, edit, &model_error)) {
          if (error != nullptr) *error = Widen(model_error);
          return false;
        }
        // The view never writes Config: after a successful edit, recompute the
        // rows, the Save gate, and the dirty flag here. This handler is the one
        // owner of the edit's status line: it reports "applied", or the first
        // blocking reason when the refresh left Save closed.
        const std::wstring reason = refresh();
        window.SetStatus(EditStatusText(reason));
        return true;
      });

  // Toolbar actions: each runs one model call, then hands the fresh rows and
  // the Save gate back to the view. Add offers a placeholder html because an
  // empty one is rejected by the model, which would leave nothing to edit.
  window.SetAddHandler([&window, &model, refresh](std::wstring* error) {
    int max_slide = 0;
    for (const auto& row : model.rows()) {
      if (row.slide > max_slide) max_slide = row.slide;
    }
    std::string model_error;
    if (!model.AddTarget(max_slide + 1, "page.html", &model_error)) {
      if (error != nullptr) *error = Widen(model_error);
      return false;
    }
    const std::wstring reason = refresh();
    window.SetStatus(reason.empty() ? L"added target" : reason);
    return true;
  });

  window.SetRemoveHandler(
      [&window, &model, refresh](std::size_t index, std::wstring* error) {
        std::string model_error;
        if (!model.RemoveTarget(index, &model_error)) {
          if (error != nullptr) *error = Widen(model_error);
          return false;
        }
        const std::wstring reason = refresh();
        window.SetStatus(reason.empty() ? L"removed target" : reason);
        return true;
      });

  window.SetValidateHandler([&window, refresh](std::wstring* error) {
    // refresh() itself reports the first blocking issue, which is what the
    // status line must show when Save is closed by a global (non-row) error.
    const std::wstring reason = refresh();
    if (!reason.empty()) {
      if (error != nullptr) *error = reason;
      return false;
    }
    window.SetStatus(L"valid");
    return true;
  });

  window.SetSaveHandler([&window, &model, config_path, refresh, &watchdog_running,
                         &run_id, exe, state_dir](std::wstring* error) {
    // Defence in depth: the button and the close prompt are already gated, but
    // a direct route must never flatten a profile-based config either.
    if (model.HasProfiles()) {
      if (error != nullptr) *error = kMultiProfileSaveDisabled;
      return false;
    }
    std::string save_error;
    if (!model.SaveWithBackup(config_path, &save_error)) {
      if (error != nullptr) *error = Widen(save_error);
      return false;
    }
    // A live editor-launched run keeps reading its old file, so the save queues
    // a hot reload; the supervisor applies it at the next safe point and logs a
    // deferral until then. The status line names the queue instead of "saved".
    if (watchdog_running && !run_id.empty()) {
      DWORD reload_code = 1;
      std::wstring reload_error;
      if (RunAndWait(BuildReloadCommandLine(exe, state_dir, run_id), 5000,
                     &reload_code, &reload_error) &&
          reload_code == 0) {
        window.SetSaveSuccessStatus(L"save reload requested");
      } else {
        window.SetSaveSuccessStatus(L"saved; reload request failed");
      }
    }
    // A successful save clears dirty, so the gate closes until the next edit.
    refresh();
    return true;
  });

  // Undo/redo: the model owns the history, so after a successful restore the
  // same refresh that follows an edit recomputes the rows, the Save gate, and
  // the dirty flag. The handler owns the success status line; a rejected
  // restore (nothing to undo or redo) leaves the view's error display to name
  // the reason.
  window.SetUndoHandler([&window, &model, refresh](std::wstring* error) {
    if (!model.Undo()) {
      if (error != nullptr) *error = L"nothing to undo";
      return false;
    }
    const std::wstring reason = refresh();
    window.SetStatus(reason.empty() ? L"undone" : reason);
    return true;
  });

  window.SetRedoHandler([&window, &model, refresh](std::wstring* error) {
    if (!model.Redo()) {
      if (error != nullptr) *error = L"nothing to redo";
      return false;
    }
    const std::wstring reason = refresh();
    window.SetStatus(reason.empty() ? L"redone" : reason);
    return true;
  });

  // Start launches one detached supervisor (the same exe re-entered with
  // --supervisor) and hands its process handle to the liveness poll. The run
  // reads the file, so unsaved edits are saved first through the same gate the
  // Save button uses; a config that cannot be saved is never launched. The
  // decision reads the real single-instance state at click time: the window's
  // Start flag is cached view state, and a supervisor started by another editor
  // after the last refresh must not be launched into a refusal.
  window.SetStartHandler([&window, &model, &watchdog_child, &watchdog_running,
                          &run_id, &last_state, config_path, exe, state_dir,
                          allow_outside_folder, refresh](std::wstring* error) {
    if (watchdog_running) {
      if (error != nullptr) *error = L"the watchdog is already running";
      return false;
    }
    if (exe.empty()) {
      if (error != nullptr) *error = L"cannot locate the watchdog executable";
      return false;
    }
    const StartDecision decision = DecideStart(
        IsSupervisorRunning(state_dir), window.save_enabled(), model.dirty());
    if (decision == StartDecision::AlreadyRunning) {
      if (error != nullptr) *error = kSupervisorAlreadyRunning;
      return false;
    }
    // A profile-based config can never be saved, so it is never the run's
    // config either: the supervisor would run a file the editor does not own.
    if (model.HasProfiles()) {
      if (error != nullptr) *error = kMultiProfileSaveDisabled;
      return false;
    }
    if (decision == StartDecision::BlockedConfig) {
      // refresh() reports the first blocking reason for the dirty model.
      const std::wstring reason = refresh();
      if (error != nullptr) {
        *error = reason.empty() ? L"cannot save the config before starting"
                                : reason;
      }
      return false;
    }
    if (decision == StartDecision::NeedsSave) {
      std::string save_error;
      if (!model.SaveWithBackup(config_path, &save_error)) {
        if (error != nullptr) *error = Widen(save_error);
        return false;
      }
      // A successful save clears dirty and closes the Save gate; this refresh
      // also re-evaluates the Start gate.
      refresh();
    }
    // The save takes time, so another supervisor can appear between the probe
    // and the launch: probe the mutex once more immediately before starting,
    // and never launch a run that would only be refused.
    if (IsSupervisorRunning(state_dir)) {
      if (error != nullptr) *error = kSupervisorAlreadyRunning;
      return false;
    }

    const std::wstring new_run_id = MakeUiRunId(GetTickCount64());
    const std::wstring command = BuildSupervisorCommandLine(
        exe, config_path, state_dir, new_run_id, allow_outside_folder);
    PROCESS_INFORMATION child{};
    std::wstring launch_error;
    if (!LaunchDetached(command, &child, &launch_error)) {
      if (error != nullptr) {
        *error =
            launch_error.empty() ? L"cannot start the watchdog" : launch_error;
      }
      return false;
    }
    CloseHandle(child.hThread);
    watchdog_child = child.hProcess;
    watchdog_running = true;
    run_id = new_run_id;
    last_state.clear();
    // This refresh closes the Start gate for the life of the run.
    refresh();
    window.StartWatchdogPoll();
    window.SetStatus(WatchdogStatusText(run_id, L""));
    return true;
  });

  // Stop asks the run to shut down and reports its exit code. --stop returns
  // after the supervisor's last cleanup step, so the process is reaped here
  // before Start reopens.
  window.SetStopHandler([&window, &watchdog_child, &watchdog_running, &run_id,
                         &last_state, exe, state_dir,
                         refresh](std::wstring* error) {
    if (!watchdog_running || watchdog_child == nullptr) {
      if (error != nullptr) *error = L"no watchdog is running";
      return false;
    }
    const std::wstring command = BuildStopCommandLine(exe, state_dir, run_id);
    DWORD exit_code = 1;
    std::wstring stop_error;
    if (!RunAndWait(command, 15000, &exit_code, &stop_error)) {
      if (error != nullptr) {
        *error = stop_error.empty() ? L"cannot stop the watchdog" : stop_error;
      }
      return false;
    }
    if (exit_code != 0) {
      if (error != nullptr) {
        *error = L"watchdog stop failed (exit " +
                 std::to_wstring(exit_code) + L")";
      }
      return false;
    }
    if (WaitForSingleObject(watchdog_child, 5000) != WAIT_OBJECT_0) {
      if (error != nullptr) *error = L"the watchdog did not exit after stopping";
      return false;
    }
    // Report the supervisor's own exit code, not just that --stop returned 0:
    // a run that failed while stopping must not look like a clean stop.
    DWORD child_code = 0;
    if (GetExitCodeProcess(watchdog_child, &child_code) == FALSE) {
      child_code = 1;
    }
    CloseHandle(watchdog_child);
    watchdog_child = nullptr;
    watchdog_running = false;
    run_id.clear();
    last_state.clear();
    window.StopWatchdogPoll();
    // The state-dir mutex is gone with the supervisor, so this reopens Start.
    refresh();
    window.SetStatus(WatchdogExitText(child_code));
    return true;
  });

  // The liveness poll mirrors the run's last journalled state (the same name
  // --status prints) and reaps the child, so a supervisor that exits on its own
  // reopens Start instead of leaving the editor stuck on "started".
  window.SetWatchdogPollHandler([&window, &watchdog_child, &watchdog_running,
                                 &run_id, &last_state, refresh]() {
    if (watchdog_child == nullptr) return;
    if (WaitForSingleObject(watchdog_child, 0) == WAIT_OBJECT_0) {
      // A non-zero code means the supervisor refused or failed the run, so it
      // must not be reported as "watchdog stopped".
      DWORD child_code = 0;
      if (GetExitCodeProcess(watchdog_child, &child_code) == FALSE) {
        child_code = 1;
      }
      CloseHandle(watchdog_child);
      watchdog_child = nullptr;
      watchdog_running = false;
      run_id.clear();
      last_state.clear();
      window.StopWatchdogPoll();
      refresh();
      window.SetStatus(WatchdogExitText(child_code));
      return;
    }
    // Only a state change rewrites the line, so an edit's own status is not
    // erased on every tick.
    const std::wstring state = ReadRunState(run_id);
    if (!state.empty() && state != last_state) {
      last_state = state;
      window.SetStatus(WatchdogStatusText(run_id, state));
    }
  });

  // External edits: the watcher only notifies; the decision lives here. A dirty
  // editor keeps its edits and reports the conflict; a clean one reloads the
  // file as a new clean baseline (the undo history never crosses a reload).
  window.SetConfigChangedHandler([&window, &model, config_path, refresh,
                                  &selected_index]() {
    if (model.dirty()) {
      window.SetStatus(L"config changed on disk; save to overwrite");
      return;
    }
    std::string reload_error;
    if (!model.ReloadFromDisk(config_path, &reload_error)) {
      window.SetStatus(reload_error.empty()
                           ? L"config changed on disk; reload failed"
                           : Widen(reload_error));
      return;
    }
    selected_index.reset();
    refresh();
    window.SetStatus(L"reloaded from disk");
  });

  // Started last: it only posts the message handled above, and its destructor
  // stops the thread before the window is destroyed.
  ConfigFileWatcher watcher;
  watcher.Start(config_path.parent_path(), window.handle(),
                kConfigChangedMessage);

  window.SetRows(rows);
  window.SetStatus(model.HasProfiles()
                       ? kMultiProfileSaveDisabled
                       : Widen(ConfigStatusText(loaded, rows, config_path)));
  // One refresh derives every gate from the same model snapshot: nothing is
  // dirty before the first edit, so Save starts closed, and Start opens only
  // for a clean, runnable config that no supervisor already owns.
  refresh();
  const int result = window.RunMessageLoop();
  // The supervisor is detached on purpose (a UI crash must not kill a running
  // watchdog), so closing the editor only releases the local child handle.
  if (watchdog_child != nullptr) {
    CloseHandle(watchdog_child);
    watchdog_child = nullptr;
  }
  return result;
}

} // namespace htmlppt

#else

namespace htmlppt {

int RunConfigUi(const ConfigUiOptions&) {
  std::cerr << "error: --config-ui requires Windows\n";
  return 2;
}

} // namespace htmlppt

#endif // _WIN32
