#pragma once

#ifdef _WIN32

#include <windows.h>

#include <shellapi.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/config_editor.h"

namespace htmlppt {

// The editor's colour scheme. System follows the current user's Windows app
// mode; Light and Dark force one regardless of the system setting.
enum class ConfigWindowTheme { System, Light, Dark };

// One scheme's colours, in one place. Every themed surface (window background,
// list body, detail controls, status line) is painted from these values.
struct ConfigWindowColors {
  COLORREF background;
  COLORREF text;
  bool dark;
};

// White is the classic CLR_WINDOW default; Dark is the Windows dark surface
// with light text. Exposed constants so the tests name the exact colours.
inline constexpr ConfigWindowColors kConfigWindowLightColors{
    RGB(255, 255, 255), RGB(0, 0, 0), false};
inline constexpr ConfigWindowColors kConfigWindowDarkColors{
    RGB(32, 32, 32), RGB(242, 242, 242), true};

// True when the Windows app mode is dark. AppsUseLightTheme is a DWORD that is
// 0 for dark; 1 and any other value mean light. A missing (or unreadable)
// registry value falls back to light, so the input is an optional. Pure, so the
// mapping is testable without touching the registry.
bool DarkFromAppsUseLightTheme(std::optional<DWORD> apps_use_light_theme);

// True when the window's immersive dark title bar is on. False both when the
// flag is off and when the running Windows build has no such DWM attribute.
bool IsDarkTitleBarEnabled(HWND hwnd);

struct ConfigWindowOptions {
  std::wstring title;
  int width = 900;
  int height = 560;
  // Tests create the window hidden so a headless run never flashes a window.
  bool show = true;
  // System (the default) resolves the app mode when the window is created.
  ConfigWindowTheme theme = ConfigWindowTheme::System;
};

// The right-hand detail pane's field values, read back from the controls.
struct DetailFields {
  std::wstring slide;
  std::wstring label;
  std::wstring html;
  std::wstring assets;
  bool enabled = true;
};

// The window cannot parse or apply an edit; the app installs this seam, which
// receives the raw pane fields and reports an error on rejection.
using DetailEditHandler =
    std::function<bool(const DetailFields& fields, std::wstring* error)>;

// A toolbar action that needs no arguments (Add, Save, Validate). The handler
// runs the model action and refreshes the rows and the Save gate; it returns
// false with `error` set to reject the action. Add and Validate write the
// success status themselves; Save's "saved" is the view's.
using ToolbarHandler = std::function<bool(std::wstring* error)>;
// Remove carries the selected row's model index (DisplayRow::index).
using RemoveHandler =
    std::function<bool(std::size_t index, std::wstring* error)>;

// The WM_TIMER id of the watchdog liveness poll. Public so a test can deliver
// one tick without waiting for a real timer.
inline constexpr UINT kWatchdogPollTimerId = 7;

// The private message the config-file watcher posts: the file changed on disk.
// Public so the app and the tests can deliver the same notification the
// ReadDirectoryChangesW thread posts.
inline constexpr UINT kConfigChangedMessage = WM_APP + 1;

// The close policy: a window with no pending edits always closes; a dirty one
// only when the user answered the close prompt with Save and that save went
// through (answer_save). Cancel and a failed save keep the window and its edits
// open.
bool ShouldCloseWindow(bool dirty, bool answer_save);

// What a WM_DROPFILES drop should do: apply the dropped .html to the selected
// row, ask the user to pick a target first, or refuse a folder. Pure, so the
// accept/reject policy is testable without a real shell drop.
enum class DropDecision { ApplyHtml, RejectDirectory, RejectNoSelection };
DropDecision ClassifyDrop(bool is_directory, bool has_selection);

// The status line after a successful edit: an empty reason means the handler's
// refresh found nothing blocking, so the edit was applied; otherwise the reason
// is the first blocking error and is reported verbatim. The edit handler is the
// only writer, so an unchanged config never gets a blanket "applied".
std::wstring EditStatusText(const std::wstring& reason);

// Converts the detail pane's raw text into one model edit. The slide must parse
// as a whole number (an empty field is an error); html and label are encoded
// from UTF-16 to UTF-8. The assets mode and the enabled switch are not part of
// this conversion; TargetEditFromDetailFields adds them.
bool ParseTargetEditFields(const std::wstring& slide_text,
                           const std::wstring& html,
                           const std::wstring& label, TargetEdit* edit,
                           std::wstring* error);

// One detail-pane snapshot -> one TargetEdit: the three text fields through
// ParseTargetEditFields, plus the assets combo and the enabled switch. The
// combo's "inherit" entry becomes the edit's "inherit" clear sentinel, so the
// target's own assets block is dropped and the stage mode applies; an empty
// combo text leaves the target's block untouched.
bool TargetEditFromDetailFields(const DetailFields& fields, TargetEdit* edit,
                                std::wstring* error);

// The config editor's top-level window. The view is deliberately thin: it owns
// the window and its controls, and all edits go through ConfigEditorModel.
class ConfigWindow {
 public:
  ConfigWindow() = default;
  ConfigWindow(const ConfigWindow&) = delete;
  ConfigWindow& operator=(const ConfigWindow&) = delete;
  ~ConfigWindow();

  bool Create(const ConfigWindowOptions& options);
  // Runs until WM_QUIT; returns the quit code.
  int RunMessageLoop();
  // Posts WM_CLOSE; a dirty window prompts first and may refuse to close.
  bool Close();
  HWND handle() const { return handle_; }
  HWND list_view() const { return list_view_; }
  HWND status_view() const { return status_view_; }

  // Repopulates the table; called with the model's DisplayRows.
  void SetRows(const std::vector<DisplayRow>& rows);
  // One-line feedback under the table (load errors, save result, first issue).
  void SetStatus(const std::wstring& text);

  // Selects the list row at `row` (clamped to the model snapshot) and fills the
  // detail controls. -1 clears the selection and disables the pane.
  void SelectRow(int row);
  // Reads the detail controls back; the test seam for the pane binding.
  DetailFields detail_fields() const;
  // False when there is no selection, so the pane must not be written through.
  bool detail_enabled() const;

  // The app installs the edit handler; the view itself never touches Config.
  void SetEditHandler(DetailEditHandler handler);
  // Reports selection changes with the selected row's model index
  // (DisplayRow::index), or -1 when nothing is selected. A refresh that clears
  // a stale selection reports -1 as well, so the app never applies an edit to a
  // target that no longer exists. The window is only a view: the handler must
  // not assume it can keep the HWND alive past WM_DESTROY.
  void SetSelectionHandler(std::function<void(int)> handler);
  // Forwards the pane fields to the installed handler, if any. On success the
  // handler owns the status line (it reports "applied" or the blocking reason
  // the refresh found); on failure the view shows the handler's error text.
  void ApplyDetail();
  // The Apply button handle; the test seam for the WM_COMMAND route.
  HWND apply_button() const { return detail_apply_button_; }

  // Toolbar seams. The view only forwards: RunConfigUi installs the model
  // calls. Each handler rebuilds the rows and the Save gate itself.
  void SetAddHandler(ToolbarHandler handler);
  void SetRemoveHandler(RemoveHandler handler);
  void SetSaveHandler(ToolbarHandler handler);
  // Validate re-runs the resolver through the app so the status line can name
  // the first issue even when it has no row of its own (a bad deck, say).
  void SetValidateHandler(ToolbarHandler handler);
  // Undo/redo are keyboard actions (Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z). The app owns
  // the model call, the refresh of the rows gate and dirty flag, and the
  // success status; the view only routes the keys and reports a failure.
  void SetUndoHandler(ToolbarHandler handler);
  void SetRedoHandler(ToolbarHandler handler);

  // The watchdog toolbar actions. Start launches a detached supervisor; Stop
  // asks that run to shut down. Both handlers own their success status line
  // ("watchdog started: <id>", "watchdog stopped"); the view reports a failure.
  // Start is only reachable while the app has opened the gate: the config is
  // saved and healthy and no other supervisor owns the state directory.
  void SetStartHandler(ToolbarHandler handler);
  void SetStopHandler(ToolbarHandler handler);
  void SetStartEnabled(bool enabled);
  bool start_enabled() const { return start_enabled_; }

  // The watchdog liveness poll. While a launched supervisor is alive the app
  // starts a 500 ms window timer; each tick calls this handler, which reaps the
  // child, reopens Start, and mirrors the run state. Installing no handler
  // leaves the timer a no-op.
  void SetWatchdogPollHandler(std::function<void()> handler);
  void StartWatchdogPoll();
  void StopWatchdogPoll();

  // The config-file watcher notification. The app owns the decision (reload the
  // clean model, or report the conflict to a dirty editor); the view only
  // routes kConfigChangedMessage to the handler.
  void SetConfigChangedHandler(std::function<void()> handler);

  // The status line after a successful save. It defaults to "saved"; while an
  // editor-launched run is live, the app sets "reload requested" because the
  // save queued a hot reload instead of only writing the file.
  void SetSaveSuccessStatus(std::wstring text);
  // Routes one WM_KEYDOWN as an editor shortcut. `focus_in_edit` is true when
  // the focused control is a detail EDIT: plain Ctrl+Z then belongs to that
  // control's own text undo and must not be consumed, while Ctrl+Y and
  // Ctrl+Shift+Z still reach the editor because a single-line EDIT has no
  // native redo. Returns true when the key was consumed; false for every other
  // key, so the window never swallows unrelated input. With no handler
  // installed the shortcut is a no-op. Public: the pure routing is the test
  // seam, which avoids having to synthesise the thread's physical key state.
  bool HandleEditorKey(bool focus_in_edit, bool ctrl, bool shift, int vk);
  // The Remove confirmation asks MessageBoxW(MB_YESNO); tests replace this so a
  // headless run never blocks on a modal dialog.
  void SetRemoveConfirmHandler(std::function<bool()> handler);

  // The Save gate: the app sets this from `model.dirty() &&
  // !model.HasBlockingError(...)` after every action. The button and the flag
  // stay in step.
  void SetSaveEnabled(bool enabled);
  bool save_enabled() const { return save_enabled_; }

  // Mirrors ConfigEditorModel::dirty() so WM_CLOSE can decide whether to ask
  // before closing. The app updates this after every action, next to the Save
  // gate.
  void SetDirty(bool dirty);
  bool dirty() const { return dirty_; }
  // The close prompt asks MessageBoxW(MB_YESNOCANCEL); tests replace this so a
  // headless run never blocks on a modal dialog. Returns IDYES (save), IDNO
  // (discard), or IDCANCEL (keep the window). "Save" only runs the save handler
  // while the Save gate is open, so a blocked config is never persisted.
  void SetClosePromptHandler(std::function<int()> handler);

  // Toolbar button handles; the test seams for the WM_COMMAND routes.
  HWND add_button() const { return add_button_; }
  HWND remove_button() const { return remove_button_; }
  HWND save_button() const { return save_button_; }
  HWND validate_button() const { return validate_button_; }
  HWND start_button() const { return start_button_; }
  HWND stop_button() const { return stop_button_; }

  // The layout DPI (96 = 100%). The initial window size and every layout
  // metric scale with it; tests derive their expectations from this instead of
  // assuming 96.
  UINT dpi() const { return dpi_; }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateChildren();
  // The window's DPI-scaled UI font. Created with the window (and on every
  // WM_DPICHANGED), applied to every child, and deleted with the window.
  void CreateFontForDpi();
  void ApplyFont();
  // Paints every themed surface from colors_: the window's own erase brush,
  // the list colours, the WM_CTLCOLOR brushes, and the DWM title bar. Brushes
  // are DPI-independent, so WM_DPICHANGED only re-runs the (idempotent) apply.
  void ApplyTheme();
  // One 96-DPI logical unit in the window's current layout DPI.
  int Scale(int value) const;
  void Layout(int width, int height);
  void FillDetail(const DisplayRow& row);
  void ClearDetail();
  void UpdateDetailEnabled();
  void NotifySelection();
  bool ConfirmRemove();
  // WM_DROPFILES: fills the detail pane's HTML field from the first dropped
  // path and applies it through the edit handler when a row is selected. A
  // folder is refused with a status message; the caller frees the drop.
  void HandleDroppedFiles(HDROP drop);
  // WM_CLOSE: destroy when clean, otherwise ask and save, discard, or stay.
  void OnClose();
  int ConfirmClose();
  bool SaveBeforeClose();
  void AddTarget();
  void RemoveSelectedTarget();
  void SaveConfig();
  void ValidateConfig();
  void StartWatchdog();
  void StopWatchdog();

  HWND handle_ = nullptr;
  HWND list_view_ = nullptr;
  HWND status_view_ = nullptr;
  HWND add_button_ = nullptr;
  HWND remove_button_ = nullptr;
  HWND save_button_ = nullptr;
  HWND validate_button_ = nullptr;
  HWND start_button_ = nullptr;
  HWND stop_button_ = nullptr;
  HWND detail_slide_label_ = nullptr;
  HWND detail_slide_edit_ = nullptr;
  HWND detail_label_label_ = nullptr;
  HWND detail_label_edit_ = nullptr;
  HWND detail_html_label_ = nullptr;
  HWND detail_html_edit_ = nullptr;
  HWND detail_assets_label_ = nullptr;
  HWND detail_assets_combo_ = nullptr;
  HWND detail_enabled_check_ = nullptr;
  HWND detail_apply_button_ = nullptr;
  HFONT font_ = nullptr;
  // The resolved scheme for this window and its solid surface brush. The brush
  // is created once per scheme and deleted with the window (WM_DESTROY).
  ConfigWindowColors colors_ = kConfigWindowLightColors;
  HBRUSH background_brush_ = nullptr;
  COLORREF brush_color_ = CLR_INVALID;
  // The last snapshot from the model; the pane reads these rows, never the
  // list control's text.
  std::vector<DisplayRow> rows_;
  int selected_row_ = -1;
  // Programmatic list selection must not bounce back through WM_NOTIFY.
  bool suppress_selection_notify_ = false;
  DetailEditHandler edit_handler_;
  std::function<void(int)> selection_handler_;
  ToolbarHandler add_handler_;
  RemoveHandler remove_handler_;
  ToolbarHandler save_handler_;
  ToolbarHandler validate_handler_;
  ToolbarHandler undo_handler_;
  ToolbarHandler redo_handler_;
  ToolbarHandler start_handler_;
  ToolbarHandler stop_handler_;
  std::function<void()> watchdog_poll_handler_;
  bool watchdog_poll_running_ = false;
  std::function<void()> config_changed_handler_;
  // The app's success line for a save; "saved" unless a run is live.
  std::wstring save_success_status_ = L"saved";
  std::function<bool()> remove_confirm_handler_;
  std::function<int()> close_prompt_handler_;
  bool save_enabled_ = false;
  bool start_enabled_ = false;
  bool dirty_ = false;
  UINT dpi_ = 96;
};

} // namespace htmlppt

#endif // _WIN32
