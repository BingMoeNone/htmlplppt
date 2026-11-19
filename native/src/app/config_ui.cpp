#include "app/config_ui.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#include "core/clock.h"
#include "core/config.h"
#include "core/config_editor.h"
#include "core/deck_info.h"
#include "core/mapping_wizard.h"
#include "core/resolve.h"
#include "core/single_instance.h"
#include "core/types.h"
#include "core/utf8_path.h"
#include "app/deck_info_loader.h"
#include "app/supervisor.h"
#include "app/watchdog_launcher.h"
#include "platform/win32/config_file_watcher.h"
#include "platform/win32/config_window.h"
#include "platform/win32/shared_blackbox.h"
#include "platform/win32/wizard_window.h"
#include "platform/win32/wps_host.h"

namespace htmlppt {
namespace {

// The editor serializes the effective profile only, so a profile-based config
// cannot be saved without dropping the other profiles. Read-only is the safe v1
// behaviour; the user edits the profile blocks in the JSON itself.
constexpr wchar_t kMultiProfileSaveDisabled[] =
    L"multi-profile config: save is disabled (edit profiles in the JSON)";

// The click-time probes found a live run for this config -- this editor's own
// run is handled separately, so this is another editor's (or a manually
// launched) run, possibly with a different state dir.
constexpr wchar_t kSupervisorAlreadyRunning[] =
    L"another watchdog is already running for this config";

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
  // One ring read, not a full snapshot every 500 ms.
  const std::optional<BlackboxRecord> latest = blackbox->Latest();
  if (!latest.has_value()) return {};
  return Widen(StateName(static_cast<State>(latest->state)));
}

// The editor writes the config from the same directory the watcher watches, so
// a notification is only an external edit when the file itself no longer looks
// like the copy this editor last loaded, saved, or reloaded. Size, write time
// and a content hash are all part of the stamp: FAT/network shares keep 2 s
// timestamps and a same-length edit within that window would otherwise look
// like this editor's own write and be silently overwritten on the next save.
struct ConfigStamp {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  std::uint64_t hash = 0;
  friend bool operator==(const ConfigStamp& left, const ConfigStamp& right) {
    return left.size == right.size && left.mtime == right.mtime &&
           left.hash == right.hash;
  }
};

std::optional<ConfigStamp> ReadConfigStamp(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return std::nullopt;
  const auto mtime = std::filesystem::last_write_time(path, error);
  if (error) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  // FNV-1a over the bytes; the config is small and this only runs on a watcher
  // notification or a save, never per frame.
  std::uint64_t hash = 1469598103934665603ull;
  char byte = 0;
  while (input.get(byte)) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  return ConfigStamp{size, mtime, hash};
}

// Where a deck scan writes its thumbnails: one directory per editor process
// under the user's temp dir, so a rescan overwrites the previous deck's bitmaps
// instead of leaving a directory behind for every click. The export creates the
// directory; a failed create is reported by the scan itself.
std::filesystem::path ThumbnailDir() {
  const std::wstring suffix =
      L"htmlppt-thumbs-" + std::to_wstring(GetCurrentProcessId());
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(buffer) / suffix;
  }
  return std::filesystem::temp_directory_path() / suffix;
}

// Runs one helper command (--reload/--stop) off the UI thread. The window's
// poll timer collects the result, so a stuck supervisor can no longer freeze the
// window for the whole command budget. done_ is published last with release and
// read with acquire, which orders exit_code_/ok_/error_ before it.
class AsyncCommand {
 public:
  ~AsyncCommand() { Join(); }

  void Start(std::wstring cmdline, DWORD timeout_ms) {
    Join();
    ok_ = false;
    exit_code_ = 1;
    error_.clear();
    done_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this, cmdline = std::move(cmdline), timeout_ms] {
      ok_ = RunAndWait(cmdline, timeout_ms, &exit_code_, &error_);
      done_.store(true, std::memory_order_release);
    });
  }

  bool Running() const {
    return thread_.joinable() && !done_.load(std::memory_order_acquire);
  }
  bool Finished() const {
    return thread_.joinable() && done_.load(std::memory_order_acquire);
  }
  void Join() {
    if (thread_.joinable()) thread_.join();
  }
  bool ok() const { return ok_; }
  DWORD exit_code() const { return exit_code_; }
  const std::wstring& error() const { return error_; }

 private:
  std::thread thread_;
  std::atomic<bool> done_{false};
  bool ok_ = false;
  DWORD exit_code_ = 1;
  std::wstring error_;
};

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
  // The copy the editor believes is on disk: refreshed after every load, save,
  // and reload so a watcher notification can be attributed correctly.
  std::optional<ConfigStamp> last_stamp = ReadConfigStamp(config_path);

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

  // The deck scan's outcome, written by the export worker and read on the UI
  // thread after kThumbnailsReadyMessage; the mutex orders the two, since the
  // posted message is the only other synchronisation between them. Declared
  // before the window too: the handlers reference them, and the thread is
  // joined before the window is destroyed.
  struct ScanResult {
    std::vector<std::string> paths;
    std::string error;
    bool ok = false;
  };
  std::mutex scan_mutex;
  ScanResult scan_result;
  std::thread scan_thread;

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
  // A scan writes into the process's own temp directory; fixed for the life of
  // the window so a rescan overwrites the previous deck's bitmaps.
  const std::filesystem::path thumbnail_dir = ThumbnailDir();
  // The auto-mapping wizard's last-used HTML folder, seeded from the config's
  // own directory so the first open already proposes that folder's files.
  std::filesystem::path last_html_folder = config_path.parent_path();
  const bool allow_outside_folder = ui_options.allow_outside_folder;
  HANDLE watchdog_child = nullptr;
  bool watchdog_running = false;
  std::wstring run_id;
  std::wstring last_state;
  // Helper commands run on their own threads; the poll handler below completes
  // them. Declared after the window so their destructors join before the window
  // (and the handlers that reference them) is destroyed.
  AsyncCommand reload_command;
  AsyncCommand stop_command;

  // Recomputes the rows, the Save gate, the Start gate, and the dirty flag after
  // every action, and returns the first blocking message: a target row's error
  // when there is one, otherwise the config-wide resolver error that has no row
  // of its own (a missing or invalid deck). Empty means the config is clean.
  const auto refresh = [&window, &model, &deck, resolve_options,
                        &watchdog_running, &state_dir,
                        config_path]() -> std::wstring {
    // Read the current deck each time: an external config reload replaces it,
    // and a captured pointer would keep validating against the old deck.
    const DeckInfo* deck_info = deck.info.has_value() ? &*deck.info : nullptr;
    const std::vector<DisplayRow> rows =
        model.DisplayRows(deck_info, resolve_options);
    window.SetRows(rows);
    const bool blocked = model.HasBlockingError(deck_info, resolve_options);
    // A profile-based config is never saveable: ConfigToJson writes only the
    // effective profile, so a save would silently drop the other profiles.
    const bool profiles_blocked = model.HasProfiles();
    window.SetSaveEnabled(model.dirty() && !blocked && !profiles_blocked);
    // Start needs a runnable config and a free run. Two probes cover every way a
    // run can already exist: the state-dir mutex for an editor-launched run, and
    // the per-config watcher.lock for a worker whose state dir is elsewhere (a
    // run launched by another editor or with another --state-dir). A second
    // supervisor must never be started into a refusal (spec 9.6).
    const bool supervisor_running = IsSupervisorRunning(state_dir);
    const bool watchdog_live = IsWatchdogLiveForConfig(config_path);
    window.SetStartEnabled(!blocked && !profiles_blocked && !watchdog_running &&
                           !supervisor_running && !watchdog_live);
    // Scanning opens and closes the deck through the shared WPS instance, which
    // ends a show the watchdog is presenting; close the gate for every liveness
    // source, not just this editor's child.
    window.SetScanEnabled(
        CanScanDeck(watchdog_running, supervisor_running, watchdog_live));
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

  window.SetSaveHandler([&window, &model, config_path, refresh, &last_stamp,
                         &watchdog_running, &run_id, exe,
                         state_dir, &reload_command](std::wstring* error) {
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
    // This is the editor's own write: stamp it before the notification arrives
    // so the watcher does not mistake it for an external edit.
    last_stamp = ReadConfigStamp(config_path);
    // A live editor-launched run keeps reading its old file, so the save queues
    // a hot reload; the supervisor applies it at the next safe point and logs a
    // deferral until then. The request is sent asynchronously: a stuck supervisor
    // must not freeze the editor while the helper runs. The reload status is set
    // through SetSaveSuccessStatus, which the view prints after the handler
    // returns: a direct SetStatus here would be overwritten.
    if (watchdog_running && !run_id.empty()) {
      if (reload_command.Running()) {
        window.SetSaveSuccessStatus(L"saved; a reload is already in flight");
      } else {
        reload_command.Start(BuildReloadCommandLine(exe, state_dir, run_id),
                             10000);
        window.SetSaveSuccessStatus(L"saved; reload queued");
      }
    } else {
      window.SetSaveSuccessStatus(L"saved");
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

  // Scan deck exports one thumbnail per slide of the model's deck. The COM
  // export is slow and must not freeze the editor, so it runs on a worker
  // thread that owns its own COM apartment and WPS host; the worker only
  // prepares the paths, and the UI thread repaints the list when it posts
  // kThumbnailsReadyMessage. The deck comes from the live model, so a scan
  // follows the config being edited rather than the file on disk.
  window.SetScanHandler([&window, &model, &scan_mutex, &scan_result,
                         &scan_thread, &watchdog_running, state_dir, config_path,
                         thumbnail_dir](std::wstring* error) {
    if (scan_thread.joinable()) {
      if (error != nullptr) *error = L"a deck scan is already running";
      return false;
    }
    // The export opens and closes the deck through the shared WPS instance, so
    // it would end a show the watchdog is presenting. Probe at click time: the
    // button's enabled state can be stale, and a run started by another editor
    // never touches this editor's child flag.
    if (!CanScanDeck(watchdog_running, IsSupervisorRunning(state_dir),
                     IsWatchdogLiveForConfig(config_path))) {
      if (error != nullptr) {
        *error = L"stop the watchdog before scanning the deck";
      }
      return false;
    }
    const std::filesystem::path deck = ResolveDeckPath(model.config());
    if (deck.empty()) {
      if (error != nullptr) *error = L"the config has no deck to scan";
      return false;
    }
    const HWND handle = window.handle();
    if (handle == nullptr) {
      if (error != nullptr) *error = L"the config window is not open";
      return false;
    }
    scan_thread = std::thread([handle, deck, thumbnail_dir, &scan_mutex,
                               &scan_result]() {
      // A rescan starts clean, so a deck that shrank cannot leave stale
      // slide-NN.bmp files behind for a later scan to misread.
      RemoveDirectoryRecursively(thumbnail_dir);
      SteadyClock clock;
      WpsHostConfig host_config;
      // One attempt: the scan is a user-initiated extra, and a failed launch
      // must report quickly instead of retrying behind the editor.
      host_config.launch_attempts = 1;
      WpsHost host(clock, host_config);
      std::vector<std::string> paths;
      std::string export_error;
      const bool ok = host.ExportSlideThumbnails(deck, thumbnail_dir, &paths,
                                                 &export_error);
      host.Release();
      {
        const std::lock_guard<std::mutex> lock(scan_mutex);
        scan_result.ok = ok;
        scan_result.paths = std::move(paths);
        scan_result.error = std::move(export_error);
      }
      // Posted last: the message is the worker's "results are ready" edge.
      PostMessageW(handle, kThumbnailsReadyMessage, 0, 0);
    });
    window.SetStatus(L"scanning deck...");
    return true;
  });

  // The worker finished: read the prepared paths and hand them to the view. The
  // join is immediate because posting the message was the worker's last action;
  // it also means no scan thread can outlive the window. A failed scan clears
  // the previous deck's thumbnails rather than leaving them under new rows.
  window.SetThumbnailsHandler([&window, &scan_mutex, &scan_result,
                               &scan_thread]() {
    if (scan_thread.joinable()) scan_thread.join();
    ScanResult result;
    {
      const std::lock_guard<std::mutex> lock(scan_mutex);
      result = scan_result;
    }
    if (!result.ok) {
      window.SetThumbnails({});
      window.SetStatus(result.error.empty() ? L"deck scan failed"
                                           : Widen(result.error));
      return;
    }
    window.SetThumbnails(result.paths);
    window.SetStatus(L"scanned " + std::to_wstring(result.paths.size()) +
                     L" slides");
  });

  // The auto-mapping wizard (§9.3): proposes slide -> HTML pairs from the scanned
  // deck and an HTML folder, previews them with a per-row include, and imports
  // the checked rows as one undoable model edit. The deck is the same info the
  // rest of the editor validates with; without it the wizard could only guess
  // slide numbers, so it refuses and says so. The last folder a successful
  // import used seeds the next wizard (the config's own directory before that).
  window.SetWizardHandler([&window, &model, &deck, &last_html_folder,
                           refresh](std::wstring* error) {
    const DeckInfo* deck_info = deck.info.has_value() ? &*deck.info : nullptr;
    if (deck_info == nullptr || deck_info->slides.empty()) {
      if (error != nullptr) *error = L"the deck has no slide information";
      return false;
    }
    WizardWindowOptions options;
    // The wizard shows itself in Show(), which owns the modal loop.
    options.show = false;
    options.owner = window.handle();
    options.title = L"htmlppt mapping wizard";
    options.html_folder = last_html_folder;
    options.slides.reserve(deck_info->slides.size());
    for (const DeckSlideInfo& slide : deck_info->slides) {
      options.slides.push_back(WizardSlide{slide.index, slide.title});
    }

    WizardWindow wizard;
    if (!wizard.Create(options)) {
      if (error != nullptr) *error = L"cannot open the mapping wizard";
      return false;
    }
    if (!wizard.Show()) {
      // Cancel/close is not an error, but no import happened: report it and
      // leave the model and the last-used folder untouched.
      window.SetStatus(L"mapping wizard cancelled");
      return true;
    }
    const std::vector<WizardPair>& pairs = wizard.result().pairs;
    std::string model_error;
    int skipped = 0;
    if (!model.ImportTargets(pairs, &model_error, &skipped)) {
      if (error != nullptr) *error = Widen(model_error);
      return false;
    }
    if (!wizard.html_folder().empty()) last_html_folder = wizard.html_folder();
    // The import is one model edit; the usual refresh recomputes the rows, the
    // Save gate, and the dirty flag. A blocking reason always wins the status
    // line, exactly like the other editing actions.
    const std::wstring reason = refresh();
    const std::wstring imported =
        L"imported " + std::to_wstring(pairs.size() - skipped) + L" targets" +
        (skipped > 0 ? L" (" + std::to_wstring(skipped) + L" already mapped)"
                     : L"");
    window.SetStatus(reason.empty() ? imported : reason);
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
                          &run_id, &last_state, &last_stamp, config_path, exe,
                          state_dir, allow_outside_folder,
                          refresh](std::wstring* error) {
    if (watchdog_running) {
      if (error != nullptr) *error = L"the watchdog is already running";
      return false;
    }
    if (exe.empty()) {
      if (error != nullptr) *error = L"cannot locate the watchdog executable";
      return false;
    }
    const StartDecision decision =
        DecideStart(IsSupervisorRunning(state_dir),
                    IsWatchdogLiveForConfig(config_path), window.save_enabled(),
                    model.dirty());
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
      // Start's implicit save is still this editor's own write: stamp it, or
      // the watcher's notification arrives as an "external" edit and reloads
      // over the run that is being started.
      last_stamp = ReadConfigStamp(config_path);
      // A successful save clears dirty and closes the Save gate; this refresh
      // also re-evaluates the Start gate.
      refresh();
    }
    // The save takes time, so another supervisor can appear between the probe
    // and the launch: probe both liveness sources once more immediately before
    // starting, and never launch a run that would only be refused.
    if (IsSupervisorRunning(state_dir) ||
        IsWatchdogLiveForConfig(config_path)) {
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

  // Stop asks the run to shut down. --stop itself can take longer than the
  // supervisor's 20 s cleanup budget on a slow machine, so it runs on a helper
  // thread and the poll handler below reaps the child and reopens Start. The UI
  // thread never blocks on the command.
  window.SetStopHandler([&window, &watchdog_child, &watchdog_running, &run_id,
                         &last_state, exe, state_dir, &stop_command](
                            std::wstring* error) {
    if (!watchdog_running || watchdog_child == nullptr) {
      if (error != nullptr) *error = L"no watchdog is running";
      return false;
    }
    if (stop_command.Running()) {
      if (error != nullptr) *error = L"a stop is already in progress";
      return false;
    }
    stop_command.Start(BuildStopCommandLine(exe, state_dir, run_id), 30000);
    window.SetStatus(L"stopping watchdog...");
    return true;
  });

  // The liveness poll mirrors the run's last journalled state (the same name
  // --status prints) and reaps the child, so a supervisor that exits on its own
  // reopens Start instead of leaving the editor stuck on "started". It also
  // completes the asynchronous --reload/--stop helpers started above.
  window.SetWatchdogPollHandler([&window, &watchdog_child, &watchdog_running,
                                 &run_id, &last_state, refresh, &reload_command,
                                 &stop_command]() {
    if (reload_command.Finished()) {
      reload_command.Join();
      window.SetStatus(reload_command.ok() && reload_command.exit_code() == 0
                           ? L"save reload requested"
                           : L"saved; reload request failed");
    }
    if (stop_command.Finished()) {
      stop_command.Join();
      const bool stop_ok = stop_command.ok();
      const DWORD stop_code = stop_command.exit_code();
      if (!stop_ok) {
        window.SetStatus(stop_command.error().empty()
                             ? L"cannot stop the watchdog"
                             : L"cannot stop the watchdog: " +
                                   stop_command.error());
      } else if (stop_code != 0) {
        window.SetStatus(L"watchdog stop failed (exit " +
                         std::to_wstring(stop_code) + L")");
      } else if (watchdog_child != nullptr) {
        // Report the supervisor's own exit code, not just that --stop returned
        // 0: a run that failed while stopping must not look like a clean stop.
        if (WaitForSingleObject(watchdog_child, 5000) != WAIT_OBJECT_0) {
          window.SetStatus(L"the watchdog did not exit after stopping");
        } else {
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
          // No run is live any more, so a later save must not claim a reload.
          window.SetSaveSuccessStatus(L"saved");
          window.SetStatus(WatchdogExitText(child_code));
        }
      }
      return;
    }
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
      window.SetSaveSuccessStatus(L"saved");
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

  // External edits: the watcher only notifies; the decision lives here. The
  // notification is ignored when the file still matches the copy this editor
  // wrote or read, which is what keeps a save from reloading over itself and a
  // sibling write (the .bak, the run's state directory) from being called an
  // external edit.
  window.SetConfigChangedHandler([&window, &model, config_path, refresh,
                                  &selected_index, &last_stamp, &deck]() {
    const std::optional<ConfigStamp> stamp = ReadConfigStamp(config_path);
    if (stamp == last_stamp) return;
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
    // Never ask WPS for the deck again from this handler: the load is a
    // synchronous COM activation on the UI thread (seconds of freeze on every
    // external edit), and the deck facts only change when the deck path does.
    // A deck switch is not hot-reloadable anyway (the supervisor refuses one),
    // so drop the stale facts and ask for a reopen instead of blocking.
    const std::filesystem::path new_deck = ResolveDeckPath(model.config());
    if (deck.info.has_value() && !PathEquals(deck.info->path, new_deck)) {
      deck = DeckInfoLoad{};
      last_stamp = stamp;
      selected_index.reset();
      refresh();
      window.SetStatus(
          L"the deck changed on disk; reopen the editor to reload it");
      return;
    }
    last_stamp = stamp;
    selected_index.reset();
    refresh();
    window.SetStatus(deck.info.has_value()
                         ? L"reloaded from disk"
                         : L"reloaded from disk; deck info unavailable");
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
  // A scan worker that is still running when the loop ends posts to a window
  // that is about to be destroyed, so it is joined here before the window goes
  // away; its result is simply discarded.
  if (scan_thread.joinable()) scan_thread.join();
  // The image list holds its own copies, so the BMP files are no longer needed
  // once the editor closes.
  RemoveDirectoryRecursively(thumbnail_dir);
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
