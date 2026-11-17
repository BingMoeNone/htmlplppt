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
  // Forwards the pane fields to the installed handler, if any; the status line
  // shows "applied" or the handler's error text.
  void ApplyDetail();
  // The Apply button handle; the test seam for the WM_COMMAND route.
  HWND apply_button() const { return detail_apply_button_; }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateChildren();
  void Layout(int width, int height);
  void FillDetail(const DisplayRow& row);
  void ClearDetail();
  void UpdateDetailEnabled();

  HWND handle_ = nullptr;
  HWND list_view_ = nullptr;
  HWND status_view_ = nullptr;
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
};

} // namespace htmlppt

#endif // _WIN32
