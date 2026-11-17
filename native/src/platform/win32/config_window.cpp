#include "platform/win32/config_window.h"

#include <windows.h>

#include <commctrl.h>

#include <utility>

namespace htmlppt {
namespace {

constexpr wchar_t kWindowClass[] = L"HtmlpptConfigWindow";

// Stable id so the Apply button's BN_CLICKED can be routed in WM_COMMAND.
constexpr int kApplyButtonId = 1001;

// Master-detail layout: the list fills the left column, the detail pane is a
// fixed right-hand column, and the status line spans the bottom.
constexpr int kStatusHeight = 24;
constexpr int kDetailWidth = 300;
constexpr int kDetailMargin = 12;

// The model carries UTF-8; the list control stores UTF-16. Widen as a real
// UTF-8 -> UTF-16 conversion so non-ASCII labels and paths survive intact.
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

// Control text, including the selected item's text for a drop-down list combo.
std::wstring TextOf(HWND control) {
  if (control == nullptr) return {};
  const int length = GetWindowTextLengthW(control);
  if (length <= 0) return {};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(control, text.data(), length + 1);
  if (copied <= 0) return {};
  text.resize(static_cast<std::size_t>(copied));
  return text;
}

} // namespace

ConfigWindow::~ConfigWindow() {
  if (handle_ != nullptr) {
    DestroyWindow(handle_);
    handle_ = nullptr;
  }
}

bool ConfigWindow::Create(const ConfigWindowOptions& options) {
  if (handle_ != nullptr) return false;

  // Common Controls v6 is initialized once per process; the ListView arrives in
  // a later task but the control class must be available before creation.
  static bool controls_ready = false;
  if (!controls_ready) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);
    controls_ready = true;
  }

  static ATOM window_class = 0;
  if (window_class == 0) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ConfigWindow::WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // IDC_ARROW expands to a narrow resource name without UNICODE defined.
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    window_class = RegisterClassExW(&wc);
    if (window_class == 0) return false;
  }

  handle_ = CreateWindowExW(
      0, kWindowClass, options.title.c_str(), WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, options.width, options.height, nullptr,
      nullptr, GetModuleHandleW(nullptr), this);
  if (handle_ == nullptr) return false;
  CreateChildren();
  RECT client{};
  GetClientRect(handle_, &client);
  Layout(client.right - client.left, client.bottom - client.top);
  if (options.show) {
    ShowWindow(handle_, SW_SHOW);
    UpdateWindow(handle_);
  }
  return true;
}

void ConfigWindow::CreateChildren() {
  list_view_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      0, 0, 0, 0, handle_, nullptr, GetModuleHandleW(nullptr), nullptr);
  status_view_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                 0, 0, 0, 0, handle_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);

  const wchar_t* headings[] = {L"SLIDE", L"LABEL", L"HTML", L"ASSETS",
                               L"STATE"};
  const int widths[] = {70, 180, 380, 90, 110};
  for (int i = 0; i < 5; ++i) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(headings[i]);
    column.cx = widths[i];
    column.iSubItem = i;
    ListView_InsertColumn(list_view_, i, &column);
  }

  // Detail pane: STATIC labels plus EDITs for slide/label/html, a drop-down
  // list for the assets mode, an enabled checkbox, and Apply.
  const DWORD static_style = WS_CHILD | WS_VISIBLE | SS_LEFT;
  const DWORD edit_style =
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
  const auto make_label = [&](const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text, static_style, 0, 0, 0, 0,
                           handle_, nullptr, GetModuleHandleW(nullptr), nullptr);
  };
  const auto make_edit = [&]() {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", edit_style, 0, 0, 0,
                           0, handle_, nullptr, GetModuleHandleW(nullptr),
                           nullptr);
  };
  detail_slide_label_ = make_label(L"SLIDE");
  detail_slide_edit_ = make_edit();
  detail_label_label_ = make_label(L"LABEL");
  detail_label_edit_ = make_edit();
  detail_html_label_ = make_label(L"HTML");
  detail_html_edit_ = make_edit();
  detail_assets_label_ = make_label(L"ASSETS");
  detail_assets_combo_ = CreateWindowExW(
      0, L"COMBOBOX", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
      0, 0, 0, handle_, nullptr, GetModuleHandleW(nullptr), nullptr);
  const wchar_t* assets_modes[] = {L"inherit", L"direct", L"file", L"tree"};
  for (const wchar_t* mode : assets_modes) {
    SendMessageW(detail_assets_combo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(mode));
  }
  SendMessageW(detail_assets_combo_, CB_SETCURSEL, 0, 0);
  detail_enabled_check_ =
      CreateWindowExW(0, L"BUTTON", L"Enabled",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0,
                      0, 0, 0, handle_, nullptr, GetModuleHandleW(nullptr),
                      nullptr);
  detail_apply_button_ =
      CreateWindowExW(0, L"BUTTON", L"Apply",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0,
                      0, 0, handle_,
                      reinterpret_cast<HMENU>(
                          static_cast<INT_PTR>(kApplyButtonId)),
                      GetModuleHandleW(nullptr), nullptr);
  UpdateDetailEnabled();
}

void ConfigWindow::Layout(int width, int height) {
  if (handle_ == nullptr || list_view_ == nullptr || status_view_ == nullptr) {
    return;
  }
  const int list_width = width > kDetailWidth ? width - kDetailWidth : 0;
  const int body_height = height - kStatusHeight;
  MoveWindow(list_view_, 0, 0, list_width, body_height > 0 ? body_height : 0,
             TRUE);
  MoveWindow(status_view_, 0, height - kStatusHeight, width, kStatusHeight,
             TRUE);

  const int pane_left = width > kDetailWidth ? width - kDetailWidth : 0;
  const int field_left = pane_left + kDetailMargin;
  const int field_width = kDetailWidth - 2 * kDetailMargin;
  const auto place = [&](HWND control, int top, int control_height) {
    if (control == nullptr) return;
    MoveWindow(control, field_left, top, field_width, control_height, TRUE);
  };
  place(detail_slide_label_, 8, 16);
  place(detail_slide_edit_, 24, 24);
  place(detail_label_label_, 56, 16);
  place(detail_label_edit_, 72, 24);
  place(detail_html_label_, 104, 16);
  place(detail_html_edit_, 120, 24);
  place(detail_assets_label_, 152, 16);
  // The combo's height includes the drop-down list, so it owns a taller band.
  place(detail_assets_combo_, 168, 120);
  place(detail_enabled_check_, 296, 24);
  if (detail_apply_button_ != nullptr) {
    MoveWindow(detail_apply_button_, field_left, 328, 90, 28, TRUE);
  }
}

void ConfigWindow::SetRows(const std::vector<DisplayRow>& rows) {
  rows_ = rows;
  if (list_view_ == nullptr) {
    selected_row_ = -1;
    ClearDetail();
    UpdateDetailEnabled();
    return;
  }
  // Programmatic item mutation/selection must not bounce back as a user
  // selection through LVN_ITEMCHANGED.
  const bool previous_suppress = suppress_selection_notify_;
  suppress_selection_notify_ = true;
  SendMessageW(list_view_, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list_view_);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const std::wstring slide = std::to_wstring(row.slide);
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(i);
    item.iSubItem = 0;
    item.pszText = const_cast<wchar_t*>(slide.c_str());
    // The model's own index, not the loop position: later tasks address the
    // model through this value.
    item.lParam = static_cast<LPARAM>(row.index);
    const int inserted = ListView_InsertItem(list_view_, &item);
    if (inserted < 0) continue;
    const auto set_text = [&](int column, const std::string& text) {
      const std::wstring wide = Widen(text);
      ListView_SetItemText(list_view_, inserted, column,
                           const_cast<wchar_t*>(wide.c_str()));
    };
    set_text(1, row.label);
    set_text(2, row.html);
    set_text(3, row.assets_mode);
    set_text(4, row.issue.empty() ? row.state : row.state + ": " + row.issue);
  }
  SendMessageW(list_view_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(list_view_, nullptr, TRUE);

  // A selection that no longer exists must not survive the repopulation.
  if (selected_row_ >= static_cast<int>(rows_.size())) selected_row_ = -1;
  if (selected_row_ >= 0) {
    ListView_SetItemState(list_view_, selected_row_,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    FillDetail(rows_[static_cast<std::size_t>(selected_row_)]);
  } else {
    ClearDetail();
  }
  suppress_selection_notify_ = previous_suppress;
  UpdateDetailEnabled();
}

void ConfigWindow::SetStatus(const std::wstring& text) {
  if (status_view_ == nullptr) return;
  SetWindowTextW(status_view_, text.c_str());
}

void ConfigWindow::SelectRow(int row) {
  int clamped = row;
  if (clamped < -1) clamped = -1;
  if (clamped >= static_cast<int>(rows_.size())) {
    clamped = static_cast<int>(rows_.size()) - 1;
  }
  selected_row_ = clamped;
  // Keep the list's own selection in step without re-entering SelectRow.
  const bool previous_suppress = suppress_selection_notify_;
  suppress_selection_notify_ = true;
  if (list_view_ != nullptr) {
    if (selected_row_ >= 0) {
      ListView_SetItemState(list_view_, selected_row_,
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(list_view_, selected_row_, FALSE);
    } else {
      ListView_SetItemState(list_view_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    }
  }
  suppress_selection_notify_ = previous_suppress;
  if (selected_row_ >= 0) {
    FillDetail(rows_[static_cast<std::size_t>(selected_row_)]);
  } else {
    ClearDetail();
  }
  UpdateDetailEnabled();
}

DetailFields ConfigWindow::detail_fields() const {
  DetailFields fields;
  fields.slide = TextOf(detail_slide_edit_);
  fields.label = TextOf(detail_label_edit_);
  fields.html = TextOf(detail_html_edit_);
  fields.assets = TextOf(detail_assets_combo_);
  fields.enabled =
      SendMessageW(detail_enabled_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  return fields;
}

bool ConfigWindow::detail_enabled() const {
  return selected_row_ >= 0 && detail_slide_edit_ != nullptr &&
         IsWindowEnabled(detail_slide_edit_) != 0;
}

void ConfigWindow::SetEditHandler(DetailEditHandler handler) {
  edit_handler_ = std::move(handler);
}

void ConfigWindow::ApplyDetail() {
  if (!edit_handler_) return;
  const DetailFields fields = detail_fields();
  std::wstring error;
  if (edit_handler_(fields, &error)) {
    SetStatus(L"applied");
  } else {
    SetStatus(error);
  }
}

void ConfigWindow::FillDetail(const DisplayRow& row) {
  const std::wstring slide = std::to_wstring(row.slide);
  SetWindowTextW(detail_slide_edit_, slide.c_str());
  const std::wstring label = Widen(row.label);
  SetWindowTextW(detail_label_edit_, label.c_str());
  const std::wstring html = Widen(row.html);
  SetWindowTextW(detail_html_edit_, html.c_str());
  // An inherited stage mode is presented as "inherit"; only a target that owns
  // its assets block shows the effective mode as a choice.
  int selection = 0;  // "inherit" is the first entry
  if (row.assets_explicit && detail_assets_combo_ != nullptr) {
    const std::wstring assets = Widen(row.assets_mode);
    if (!assets.empty()) {
      const LRESULT found = SendMessageW(
          detail_assets_combo_, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
          reinterpret_cast<LPARAM>(assets.c_str()));
      if (found != CB_ERR) selection = static_cast<int>(found);
    }
  }
  SendMessageW(detail_assets_combo_, CB_SETCURSEL,
               static_cast<WPARAM>(selection), 0);
  SendMessageW(detail_enabled_check_, BM_SETCHECK,
               row.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
}

void ConfigWindow::ClearDetail() {
  SetWindowTextW(detail_slide_edit_, L"");
  SetWindowTextW(detail_label_edit_, L"");
  SetWindowTextW(detail_html_edit_, L"");
  SendMessageW(detail_assets_combo_, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
  SendMessageW(detail_enabled_check_, BM_SETCHECK, BST_UNCHECKED, 0);
}

void ConfigWindow::UpdateDetailEnabled() {
  const BOOL enabled = selected_row_ >= 0 ? TRUE : FALSE;
  HWND controls[] = {detail_slide_edit_, detail_label_edit_,
                     detail_html_edit_,  detail_assets_combo_,
                     detail_enabled_check_, detail_apply_button_};
  for (HWND control : controls) {
    if (control != nullptr) EnableWindow(control, enabled);
  }
}

int ConfigWindow::RunMessageLoop() {
  MSG message;
  for (;;) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == -1) return -1;
    if (result == 0) return static_cast<int>(message.wParam);
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

bool ConfigWindow::Close() {
  if (handle_ == nullptr) return false;
  return PostMessageW(handle_, WM_CLOSE, 0, 0) != FALSE;
}

LRESULT CALLBACK ConfigWindow::WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                          LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<ConfigWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) return self->HandleMessage(hwnd, message, wparam, lparam);
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ConfigWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      Layout(static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam)));
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == kApplyButtonId && HIWORD(wparam) == BN_CLICKED) {
        ApplyDetail();
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header != nullptr && header->hwndFrom == list_view_ &&
          header->code == LVN_ITEMCHANGED) {
        auto* changed = reinterpret_cast<NMLISTVIEW*>(lparam);
        // Only an actual select/deselect toggles the pane; a focus-only change
        // must not discard what the user typed into the detail fields.
        const bool selection_changed =
            (changed->uChanged & LVIF_STATE) != 0 &&
            ((changed->uOldState ^ changed->uNewState) & LVIS_SELECTED) != 0;
        if (!suppress_selection_notify_ && selection_changed) {
          if ((changed->uNewState & LVIS_SELECTED) != 0) {
            SelectRow(changed->iItem);
          } else if (changed->iItem == selected_row_) {
            // The row the pane shows was deselected: clear it rather than
            // leaving an enabled pane bound to a row no longer selected.
            SelectRow(-1);
          }
        }
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_DESTROY:
      // Child windows are gone with the parent; never hold stale HWNDs.
      handle_ = nullptr;
      list_view_ = nullptr;
      status_view_ = nullptr;
      detail_slide_label_ = nullptr;
      detail_slide_edit_ = nullptr;
      detail_label_label_ = nullptr;
      detail_label_edit_ = nullptr;
      detail_html_label_ = nullptr;
      detail_html_edit_ = nullptr;
      detail_assets_label_ = nullptr;
      detail_assets_combo_ = nullptr;
      detail_enabled_check_ = nullptr;
      detail_apply_button_ = nullptr;
      rows_.clear();
      selected_row_ = -1;
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

} // namespace htmlppt
