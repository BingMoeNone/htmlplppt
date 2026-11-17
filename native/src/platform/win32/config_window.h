#pragma once

#ifdef _WIN32

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/config_editor.h"

namespace htmlppt {

struct ConfigWindowOptions {
  std::wstring title;
  int width = 900;
  int height = 560;
  // Tests create the window hidden so a headless run never flashes a window.
  bool show = true;
};

// The right-hand detail pane's field values, read back from the controls.
struct DetailFields {
  std::wstring slide;
  std::wstring label;
  std::wstring html;
  std::wstring assets;
  bool enabled = true;
};

// The window cannot parse or apply an edit; Task 3 installs this seam, which
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
  // Posts WM_CLOSE (the window may refuse while dirty; see Task 8).
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

  // Task 3 installs the edit handler; the view itself never touches Config.
  void SetEditHandler(DetailEditHandler handler);
  // Reports selection changes with the selected row's model index
  // (DisplayRow::index), or -1 when nothing is selected. A refresh that clears
  // a stale selection reports -1 as well, so the app never applies an edit to a
  // target that no longer exists. The window is only a view: the handler must
  // not assume it can keep the HWND alive past WM_DESTROY.
  void SetSelectionHandler(std::function<void(int)> handler);
  // Forwards the pane fields to the installed handler, if any; the status line
  // shows "applied" or the handler's error text.
  void ApplyDetail();
  // The Apply button handle; the test seam for the WM_COMMAND route.
  HWND apply_button() const { return detail_apply_button_; }

  // Toolbar seams (Task 4). The view only forwards: RunConfigUi installs the
  // model calls. Each handler rebuilds the rows and the Save gate itself.
  void SetAddHandler(ToolbarHandler handler);
  void SetRemoveHandler(RemoveHandler handler);
  void SetSaveHandler(ToolbarHandler handler);
  // Validate re-runs the resolver through the app so the status line can name
  // the first issue even when it has no row of its own (a bad deck, say).
  void SetValidateHandler(ToolbarHandler handler);
  // The Remove confirmation asks MessageBoxW(MB_YESNO); tests replace this so a
  // headless run never blocks on a modal dialog.
  void SetRemoveConfirmHandler(std::function<bool()> handler);

  // The Save gate: the app sets this from `model.dirty() &&
  // !model.HasBlockingError(...)` after every action. The button and the flag
  // stay in step.
  void SetSaveEnabled(bool enabled);
  bool save_enabled() const { return save_enabled_; }

  // Toolbar button handles; the test seams for the WM_COMMAND routes.
  HWND add_button() const { return add_button_; }
  HWND remove_button() const { return remove_button_; }
  HWND save_button() const { return save_button_; }
  HWND validate_button() const { return validate_button_; }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateChildren();
  void Layout(int width, int height);
  void FillDetail(const DisplayRow& row);
  void ClearDetail();
  void UpdateDetailEnabled();
  void NotifySelection();
  bool ConfirmRemove();
  void AddTarget();
  void RemoveSelectedTarget();
  void SaveConfig();
  void ValidateConfig();

  HWND handle_ = nullptr;
  HWND list_view_ = nullptr;
  HWND status_view_ = nullptr;
  HWND add_button_ = nullptr;
  HWND remove_button_ = nullptr;
  HWND save_button_ = nullptr;
  HWND validate_button_ = nullptr;
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
  std::function<bool()> remove_confirm_handler_;
  bool save_enabled_ = false;
};

} // namespace htmlppt

#endif // _WIN32
