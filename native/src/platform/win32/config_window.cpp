#include "platform/win32/config_window.h"

#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <charconv>
#include <optional>
#include <system_error>
#include <utility>

#include "platform/win32/utf8_win32.h"

namespace htmlppt {
namespace {

constexpr wchar_t kWindowClass[] = L"HtmlpptConfigWindow";

// Stable ids so each button's BN_CLICKED can be routed in WM_COMMAND.
constexpr int kApplyButtonId = 1001;
constexpr int kAddButtonId = 1002;
constexpr int kRemoveButtonId = 1003;
constexpr int kSaveButtonId = 1004;
constexpr int kValidateButtonId = 1005;
constexpr int kStartButtonId = 1006;
constexpr int kStopButtonId = 1007;

// How often the liveness poll checks a launched supervisor. Fast enough that a
// crashed supervisor reopens Start promptly, slow enough to be free.
constexpr UINT kWatchdogPollIntervalMs = 500;

// The assets combo's first entry: the target keeps inheriting the stage mode.
constexpr wchar_t kInheritAssets[] = L"inherit";

// Master-detail layout: the toolbar spans the top, the list fills the left
// column, the detail pane is a fixed right-hand column, and the status line
// spans the bottom.
constexpr int kStatusHeight = 24;
constexpr int kToolbarHeight = 36;
constexpr int kToolbarMargin = 8;
constexpr int kToolbarButtonWidth = 88;
constexpr int kToolbarButtonHeight = 28;
constexpr int kToolbarGap = 8;
constexpr int kDetailWidth = 300;
constexpr int kDetailMargin = 12;

// The pane's Apply button sits below the fields at this offset, with a small
// bottom margin before the status line. A window smaller than the sum below
// would clip the pane, so WM_GETMINMAXINFO refuses to shrink past it.
constexpr int kPaneApplyTop = 328;
constexpr int kPaneApplyHeight = 28;
constexpr int kPaneBottomMargin = 8;
// The minimum width fits the whole toolbar: margin + six 88 px buttons + five
// gaps + a margin, with room to spare.
constexpr int kMinClientWidth = 600;
constexpr int kMinClientHeight =
    kToolbarHeight + kPaneApplyTop + kPaneApplyHeight + kPaneBottomMargin +
    kStatusHeight;

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

// DWMWA_USE_IMMERSIVE_DARK_MODE as documented (20) and the pre-20H1 number the
// attribute used (19). A local constant keeps the build independent of how new
// the SDK's dwmapi.h is.
constexpr DWORD kDarkModeAttribute = 20;
constexpr DWORD kDarkModeAttributeLegacy = 19;

// The current user's AppsUseLightTheme value; nullopt when the key or the value
// is missing/unreadable, which the caller treats as light.
std::optional<DWORD> ReadAppsUseLightTheme() {
  constexpr wchar_t kPersonalize[] =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalize, 0, KEY_QUERY_VALUE,
                    &key) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  DWORD value = 0;
  DWORD size = sizeof value;
  DWORD type = 0;
  const LONG status =
      RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                       reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_DWORD ||
      size != sizeof value) {
    return std::nullopt;
  }
  return value;
}

// System resolves through the user's app mode; Light and Dark are fixed.
ConfigWindowColors ColorsFor(ConfigWindowTheme theme) {
  switch (theme) {
    case ConfigWindowTheme::Dark:
      return kConfigWindowDarkColors;
    case ConfigWindowTheme::Light:
      return kConfigWindowLightColors;
    case ConfigWindowTheme::System:
      break;
  }
  return DarkFromAppsUseLightTheme(ReadAppsUseLightTheme())
             ? kConfigWindowDarkColors
             : kConfigWindowLightColors;
}

// Flips the DWM immersive dark title bar. Attribute 20 is the current spelling;
// older builds only know 19, so fall back once. A build without either cannot
// honour the request, which is not an error.
void SetDarkTitleBar(HWND hwnd, bool dark) {
  if (hwnd == nullptr) return;
  const BOOL enabled = dark ? TRUE : FALSE;
  if (SUCCEEDED(DwmSetWindowAttribute(hwnd, kDarkModeAttribute, &enabled,
                                      sizeof enabled))) {
    return;
  }
  DwmSetWindowAttribute(hwnd, kDarkModeAttributeLegacy, &enabled,
                        sizeof enabled);
}

// True when focus is in an EDIT control: the detail pane's text fields, or the
// list's label edit if the view ever grows one (Win32 registers the class as
// "Edit"). The comparison is case-insensitive so both spellings match.
bool FocusIsEdit() {
  const HWND focus = GetFocus();
  if (focus == nullptr) return false;
  wchar_t class_name[16] = {};
  if (GetClassNameW(focus, class_name, 16) <= 0) return false;
  return lstrcmpiW(class_name, L"Edit") == 0;
}

} // namespace

bool DarkFromAppsUseLightTheme(std::optional<DWORD> apps_use_light_theme) {
  return apps_use_light_theme.has_value() && *apps_use_light_theme == 0;
}

bool IsDarkTitleBarEnabled(HWND hwnd) {
  if (hwnd == nullptr) return false;
  BOOL value = FALSE;
  HRESULT result =
      DwmGetWindowAttribute(hwnd, kDarkModeAttribute, &value, sizeof value);
  if (FAILED(result)) {
    result = DwmGetWindowAttribute(hwnd, kDarkModeAttributeLegacy, &value,
                                   sizeof value);
  }
  return SUCCEEDED(result) && value != FALSE;
}

bool ShouldCloseWindow(bool dirty, bool answer_save) {
  return !dirty || answer_save;
}

DropDecision ClassifyDrop(bool is_directory, bool has_selection) {
  // A folder is never an .html file; refusing it wins over "no selection" so
  // the status line names the real problem.
  if (is_directory) return DropDecision::RejectDirectory;
  if (!has_selection) return DropDecision::RejectNoSelection;
  return DropDecision::ApplyHtml;
}

std::wstring EditStatusText(const std::wstring& reason) {
  return reason.empty() ? L"applied" : reason;
}

bool ParseTargetEditFields(const std::wstring& slide_text,
                           const std::wstring& html,
                           const std::wstring& label, TargetEdit* edit,
                           std::wstring* error) {
  const auto fail = [error](const wchar_t* message) {
    if (error != nullptr) *error = message;
    return false;
  };
  if (edit == nullptr) return fail(L"no edit to fill");
  if (slide_text.empty()) return fail(L"slide must not be empty");

  // std::from_chars has no wide overload. The slide field holds ASCII digits,
  // so an exact narrowing keeps from_chars' "consume the whole field" check:
  // anything non-digit, including surrounding spaces, fails below.
  std::string ascii;
  ascii.reserve(slide_text.size());
  for (const wchar_t ch : slide_text) {
    if (ch > 0x7F) return fail(L"slide must be a whole number");
    ascii.push_back(static_cast<char>(ch));
  }
  int slide = 0;
  const char* first = ascii.data();
  const char* last = first + ascii.size();
  const std::from_chars_result parsed = std::from_chars(first, last, slide);
  if (parsed.ec != std::errc() || parsed.ptr != last) {
    return fail(L"slide must be a whole number");
  }

  const std::optional<std::string> utf8_html = WideToUtf8(html);
  if (!utf8_html.has_value()) return fail(L"html is not valid Unicode");
  const std::optional<std::string> utf8_label = WideToUtf8(label);
  if (!utf8_label.has_value()) return fail(L"label is not valid Unicode");

  TargetEdit parsed_edit;
  parsed_edit.slide = slide;
  parsed_edit.html = *utf8_html;
  parsed_edit.label = *utf8_label;
  *edit = std::move(parsed_edit);
  if (error != nullptr) error->clear();
  return true;
}

bool TargetEditFromDetailFields(const DetailFields& fields, TargetEdit* edit,
                                std::wstring* error) {
  if (!ParseTargetEditFields(fields.slide, fields.html, fields.label, edit,
                             error)) {
    return false;
  }
  edit->enabled = fields.enabled;
  if (fields.assets.empty()) {
    // No mode chosen: leave the target's own assets block untouched.
    edit->assets_mode.clear();
    return true;
  }
  if (fields.assets == kInheritAssets) {
    // The pane's "inherit" is an explicit choice: forward the model's clear
    // sentinel so an explicit block is dropped and the stage mode applies.
    edit->assets_mode = "inherit";
    return true;
  }
  const std::optional<std::string> assets = WideToUtf8(fields.assets);
  if (!assets.has_value()) {
    if (error != nullptr) *error = L"assets mode is not valid Unicode";
    return false;
  }
  edit->assets_mode = *assets;
  return true;
}

ConfigWindow::~ConfigWindow() {
  if (handle_ != nullptr) {
    DestroyWindow(handle_);
    handle_ = nullptr;
  }
  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (background_brush_ != nullptr) {
    DeleteObject(background_brush_);
    background_brush_ = nullptr;
  }
}

bool ConfigWindow::Create(const ConfigWindowOptions& options) {
  if (handle_ != nullptr) return false;

  // Resolve the scheme before the window exists: the window paints its own
  // background and its first WM_ERASEBKGND arrives before the children (and
  // before a post-creation ApplyTheme) exist.
  colors_ = ColorsFor(options.theme);
  ApplyTheme();

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
  // The shell only delivers WM_DROPFILES to a window that accepts drops; this
  // also sets WS_EX_ACCEPTFILES on the window.
  DragAcceptFiles(handle_, TRUE);
  // The options are 96-DPI logical units. Scale the initial size to the DPI of
  // the monitor the window opened on before creating and laying out children.
  dpi_ = GetDpiForWindow(handle_);
  if (dpi_ == 0) dpi_ = 96;
  const int scaled_width = Scale(options.width);
  const int scaled_height = Scale(options.height);
  if (scaled_width != options.width || scaled_height != options.height) {
    SetWindowPos(handle_, nullptr, 0, 0, scaled_width, scaled_height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
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
  // One font for the whole window, created at the window's current DPI. It is
  // applied once every child exists, and recreated on WM_DPICHANGED.
  CreateFontForDpi();
  // Toolbar: Add / Remove / Save / Validate / Start / Stop, created first so
  // the tab order starts with the actions and the list keeps its own keyboard
  // navigation. Start and Stop are the watchdog actions; they sit after the
  // editor actions so a stray Enter never launches a run.
  const auto make_button = [&](const wchar_t* text, int id) {
    return CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, handle_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
  };
  add_button_ = make_button(L"Add", kAddButtonId);
  remove_button_ = make_button(L"Remove", kRemoveButtonId);
  save_button_ = make_button(L"Save", kSaveButtonId);
  validate_button_ = make_button(L"Validate", kValidateButtonId);
  start_button_ = make_button(L"Start", kStartButtonId);
  stop_button_ = make_button(L"Stop", kStopButtonId);

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
    column.cx = Scale(widths[i]);
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
  const wchar_t* assets_modes[] = {kInheritAssets, L"direct", L"file",
                                   L"tree"};
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
  // Nothing is dirty before the first edit, so Save starts closed.
  SetSaveEnabled(false);
  // The app opens the Start gate once the config is saved and runnable.
  SetStartEnabled(false);
  ApplyFont();
  // Children exist now, so the list colours and the DWM title bar can be set;
  // the window's own erase brush was already created before it opened.
  ApplyTheme();
}

void ConfigWindow::ApplyTheme() {
  if (background_brush_ == nullptr || brush_color_ != colors_.background) {
    if (background_brush_ != nullptr) {
      DeleteObject(background_brush_);
      background_brush_ = nullptr;
    }
    background_brush_ = CreateSolidBrush(colors_.background);
    brush_color_ = colors_.background;
  }
  // The list view owns its body colour and the per-item label background; both
  // must be themed or dark mode leaves white rectangles behind the text.
  if (list_view_ != nullptr) {
    ListView_SetBkColor(list_view_, colors_.background);
    ListView_SetTextBkColor(list_view_, colors_.background);
    ListView_SetTextColor(list_view_, colors_.text);
  }
  if (handle_ != nullptr) {
    SetDarkTitleBar(handle_, colors_.dark);
    InvalidateRect(handle_, nullptr, TRUE);
  }
}

void ConfigWindow::CreateFontForDpi() {
  // 9 pt at the current DPI; a negative height requests a character height.
  const int height = -MulDiv(9, static_cast<int>(dpi_), 72);
  HFONT new_font = CreateFontW(
      height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (new_font == nullptr) return;  // Keep the previous font rather than none.
  if (font_ != nullptr) DeleteObject(font_);
  font_ = new_font;
}

void ConfigWindow::ApplyFont() {
  if (font_ == nullptr) return;
  // Every child, including the ListView, gets the DPI-scaled font so its text
  // stays readable instead of clipping in geometry that has already grown.
  HWND children[] = {add_button_,          remove_button_,
                     save_button_,         validate_button_,
                     start_button_,        stop_button_,
                     list_view_,           status_view_,
                     detail_slide_label_,  detail_slide_edit_,
                     detail_label_label_,  detail_label_edit_,
                     detail_html_label_,   detail_html_edit_,
                     detail_assets_label_, detail_assets_combo_,
                     detail_enabled_check_, detail_apply_button_};
  for (HWND child : children) {
    if (child != nullptr) {
      SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
  }
}

int ConfigWindow::Scale(int value) const {
  return MulDiv(value, static_cast<int>(dpi_), 96);
}

void ConfigWindow::Layout(int width, int height) {
  if (handle_ == nullptr || list_view_ == nullptr || status_view_ == nullptr) {
    return;
  }
  // Toolbar band across the top; the list and the detail pane start below it.
  const int toolbar_height = Scale(kToolbarHeight);
  const int button_height = Scale(kToolbarButtonHeight);
  const int button_width = Scale(kToolbarButtonWidth);
  HWND toolbar[] = {add_button_,      remove_button_, save_button_,
                    validate_button_, start_button_,  stop_button_};
  int toolbar_x = Scale(kToolbarMargin);
  for (HWND button : toolbar) {
    if (button == nullptr) continue;
    MoveWindow(button, toolbar_x, (toolbar_height - button_height) / 2,
               button_width, button_height, TRUE);
    toolbar_x += button_width + Scale(kToolbarGap);
  }

  const int detail_width = Scale(kDetailWidth);
  const int status_height = Scale(kStatusHeight);
  const int list_width = width > detail_width ? width - detail_width : 0;
  const int body_top = toolbar_height;
  const int status_top = height - status_height;
  const int body_height = status_top - body_top;
  MoveWindow(list_view_, 0, body_top, list_width,
             body_height > 0 ? body_height : 0, TRUE);
  MoveWindow(status_view_, 0, status_top, width, status_height, TRUE);

  const int pane_left = width > detail_width ? width - detail_width : 0;
  const int field_left = pane_left + Scale(kDetailMargin);
  const int field_width = detail_width - 2 * Scale(kDetailMargin);
  const auto place = [&](HWND control, int top, int control_height) {
    if (control == nullptr) return;
    MoveWindow(control, field_left, body_top + Scale(top), field_width,
               Scale(control_height), TRUE);
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
    // Anchored above the status line as well as below the fields: a
    // programmatic resize can shrink the window below WM_GETMINMAXINFO's
    // minimum, and the Apply button must still not cover the status text.
    const int apply_height = Scale(kPaneApplyHeight);
    const int desired_top = body_top + Scale(kPaneApplyTop);
    const int lowest_top = status_top - apply_height - Scale(kPaneBottomMargin);
    int apply_top = desired_top < lowest_top ? desired_top : lowest_top;
    if (apply_top < body_top) apply_top = body_top;
    MoveWindow(detail_apply_button_, field_left, apply_top, Scale(90),
               apply_height, TRUE);
  }
}

void ConfigWindow::SetRows(const std::vector<DisplayRow>& rows) {
  rows_ = rows;
  if (list_view_ == nullptr) {
    selected_row_ = -1;
    ClearDetail();
    UpdateDetailEnabled();
    NotifySelection();
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
  NotifySelection();
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
  NotifySelection();
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

void ConfigWindow::SetSelectionHandler(std::function<void(int)> handler) {
  selection_handler_ = std::move(handler);
}

void ConfigWindow::NotifySelection() {
  if (!selection_handler_) return;
  int model_index = -1;
  if (selected_row_ >= 0 && selected_row_ < static_cast<int>(rows_.size())) {
    model_index = static_cast<int>(
        rows_[static_cast<std::size_t>(selected_row_)].index);
  }
  selection_handler_(model_index);
}

void ConfigWindow::ApplyDetail() {
  if (!edit_handler_) return;
  const DetailFields fields = detail_fields();
  std::wstring error;
  if (!edit_handler_(fields, &error)) {
    // The handler owns the success status: only it knows whether the refreshed
    // config is still blocked. The view only reports a rejection.
    SetStatus(error);
  }
}

void ConfigWindow::HandleDroppedFiles(HDROP drop) {
  // A drop carries at least one path. A failed query leaves the pane and the
  // status alone instead of applying an empty HTML field.
  const UINT needed = DragQueryFileW(drop, 0, nullptr, 0);
  if (needed == 0) return;
  // `needed` excludes the terminator, so the buffer takes one more character
  // than the text and the copy returns the text length to trim to.
  std::wstring path(static_cast<std::size_t>(needed) + 1, L'\0');
  const UINT copied = DragQueryFileW(
      drop, 0, path.data(), static_cast<UINT>(path.size()));
  if (copied == 0) return;
  path.resize(static_cast<std::size_t>(copied));

  const DWORD attributes = GetFileAttributesW(path.c_str());
  const bool is_directory =
      attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  switch (ClassifyDrop(is_directory, detail_enabled())) {
    case DropDecision::RejectDirectory:
      SetStatus(L"drop an .html file, not a folder");
      return;
    case DropDecision::RejectNoSelection:
      // Show the path so the user can pick a row and then Apply, but never
      // apply an edit with no target to apply it to.
      SetWindowTextW(detail_html_edit_, path.c_str());
      SetStatus(L"select a target first");
      return;
    case DropDecision::ApplyHtml:
      SetWindowTextW(detail_html_edit_, path.c_str());
      ApplyDetail();
      return;
  }
}

void ConfigWindow::SetAddHandler(ToolbarHandler handler) {
  add_handler_ = std::move(handler);
}

void ConfigWindow::SetRemoveHandler(RemoveHandler handler) {
  remove_handler_ = std::move(handler);
}

void ConfigWindow::SetSaveHandler(ToolbarHandler handler) {
  save_handler_ = std::move(handler);
}

void ConfigWindow::SetValidateHandler(ToolbarHandler handler) {
  validate_handler_ = std::move(handler);
}

void ConfigWindow::SetUndoHandler(ToolbarHandler handler) {
  undo_handler_ = std::move(handler);
}

void ConfigWindow::SetRedoHandler(ToolbarHandler handler) {
  redo_handler_ = std::move(handler);
}

void ConfigWindow::SetStartHandler(ToolbarHandler handler) {
  start_handler_ = std::move(handler);
}

void ConfigWindow::SetStopHandler(ToolbarHandler handler) {
  stop_handler_ = std::move(handler);
}

void ConfigWindow::SetStartEnabled(bool enabled) {
  start_enabled_ = enabled;
  if (start_button_ != nullptr) {
    EnableWindow(start_button_, enabled ? TRUE : FALSE);
  }
}

void ConfigWindow::SetWatchdogPollHandler(std::function<void()> handler) {
  watchdog_poll_handler_ = std::move(handler);
}

void ConfigWindow::SetConfigChangedHandler(std::function<void()> handler) {
  config_changed_handler_ = std::move(handler);
}

void ConfigWindow::SetSaveSuccessStatus(std::wstring text) {
  save_success_status_ = std::move(text);
}

void ConfigWindow::StartWatchdogPoll() {
  if (handle_ == nullptr || !watchdog_poll_handler_) return;
  watchdog_poll_running_ = true;
  SetTimer(handle_, kWatchdogPollTimerId, kWatchdogPollIntervalMs, nullptr);
}

void ConfigWindow::StopWatchdogPoll() {
  watchdog_poll_running_ = false;
  if (handle_ != nullptr) KillTimer(handle_, kWatchdogPollTimerId);
}

bool ConfigWindow::HandleEditorKey(bool focus_in_edit, bool ctrl, bool shift,
                                   int vk) {
  if (!ctrl) return false;
  ToolbarHandler* handler = nullptr;
  const wchar_t* fallback = nullptr;
  if (vk == 'Z') {
    if (shift) {
      handler = &redo_handler_;
      fallback = L"cannot redo";
    } else if (focus_in_edit) {
      // The focused detail EDIT owns its plain Ctrl+Z: leave the key to the
      // control's native text undo instead of undoing the whole model. Ctrl+Y
      // and Ctrl+Shift+Z stay editor redo below because a single-line EDIT has
      // no native redo.
      return false;
    } else {
      handler = &undo_handler_;
      fallback = L"cannot undo";
    }
  } else if (vk == 'Y') {
    handler = &redo_handler_;
    fallback = L"cannot redo";
  } else {
    return false;
  }
  // The shortcut belongs to the editor whether or not the app installed a
  // handler; with none, doing nothing is the correct no-op.
  if (!*handler) return true;
  std::wstring error;
  if (!(*handler)(&error)) {
    // The app owns the success status; the view only reports a rejection.
    SetStatus(error.empty() ? fallback : error);
  }
  return true;
}

void ConfigWindow::SetRemoveConfirmHandler(std::function<bool()> handler) {
  remove_confirm_handler_ = std::move(handler);
}

void ConfigWindow::SetSaveEnabled(bool enabled) {
  save_enabled_ = enabled;
  if (save_button_ != nullptr) {
    EnableWindow(save_button_, enabled ? TRUE : FALSE);
  }
}

void ConfigWindow::SetDirty(bool dirty) { dirty_ = dirty; }

void ConfigWindow::SetClosePromptHandler(std::function<int()> handler) {
  close_prompt_handler_ = std::move(handler);
}

int ConfigWindow::ConfirmClose() {
  if (close_prompt_handler_) return close_prompt_handler_();
  if (handle_ == nullptr) return IDCANCEL;
  return MessageBoxW(handle_, L"Save the changes before closing?",
                     L"htmlppt config", MB_YESNOCANCEL | MB_ICONWARNING);
}

bool ConfigWindow::SaveBeforeClose() {
  // The Save button is gated off while the config has a blocking error, and the
  // close prompt must not sneak an unrunnable config past that gate. The status
  // line already names the first blocking reason from the last refresh.
  if (!save_enabled_) return false;
  std::wstring error;
  if (!save_handler_ || !save_handler_(&error)) {
    SetStatus(error.empty() ? L"cannot save before closing" : error);
    return false;
  }
  return true;
}

void ConfigWindow::OnClose() {
  if (!dirty_) {
    DestroyWindow(handle_);
    return;
  }
  const int answer = ConfirmClose();
  if (answer == IDNO) {
    // Discard the edits and close without touching the file.
    DestroyWindow(handle_);
    return;
  }
  if (answer != IDYES) return;  // Cancel keeps the window and its edits.
  // Save first; a failed save must leave the window open with the reason in
  // the status line.
  const bool saved = SaveBeforeClose();
  if (ShouldCloseWindow(dirty_, saved)) {
    DestroyWindow(handle_);
  }
}

bool ConfigWindow::ConfirmRemove() {
  if (remove_confirm_handler_) return remove_confirm_handler_();
  if (handle_ == nullptr) return false;
  return MessageBoxW(handle_, L"Remove the selected target?",
                     L"htmlppt config", MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void ConfigWindow::AddTarget() {
  if (!add_handler_) return;
  std::wstring error;
  if (!add_handler_(&error)) {
    SetStatus(error.empty() ? L"cannot add a target" : error);
  }
}

void ConfigWindow::RemoveSelectedTarget() {
  if (!remove_handler_) return;
  if (selected_row_ < 0 || selected_row_ >= static_cast<int>(rows_.size())) {
    // The button is disabled without a selection; this guards the WM_COMMAND
    // route instead of removing an arbitrary target.
    SetStatus(L"no row selected");
    return;
  }
  if (!ConfirmRemove()) return;
  // Address the model through the snapshot's own index, not the list position.
  const std::size_t index =
      rows_[static_cast<std::size_t>(selected_row_)].index;
  std::wstring error;
  if (!remove_handler_(index, &error)) {
    SetStatus(error.empty() ? L"cannot remove the target" : error);
  }
}

void ConfigWindow::SaveConfig() {
  if (!save_handler_) return;
  std::wstring error;
  if (save_handler_(&error)) {
    SetStatus(save_success_status_);
  } else {
    SetStatus(error.empty() ? L"save failed" : error);
  }
}

void ConfigWindow::ValidateConfig() {
  if (!validate_handler_) return;
  std::wstring error;
  if (!validate_handler_(&error)) {
    SetStatus(error.empty() ? L"configuration is invalid" : error);
  }
}

void ConfigWindow::StartWatchdog() {
  // The button is disabled while the gate is closed, so this only guards the
  // synthetic WM_COMMAND route: a closed gate must never launch a run.
  if (!start_enabled_) return;
  if (!start_handler_) return;
  std::wstring error;
  if (!start_handler_(&error)) {
    SetStatus(error.empty() ? L"cannot start the watchdog" : error);
  }
}

void ConfigWindow::StopWatchdog() {
  if (!stop_handler_) return;
  std::wstring error;
  if (!stop_handler_(&error)) {
    SetStatus(error.empty() ? L"cannot stop the watchdog" : error);
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
  // Remove acts on the selection, so it follows the same rule as the pane; Add
  // and Validate are always meaningful.
  if (remove_button_ != nullptr) EnableWindow(remove_button_, enabled);
}

int ConfigWindow::RunMessageLoop() {
  MSG message;
  for (;;) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == -1) return -1;
    if (result == 0) return static_cast<int>(message.wParam);
    // The editor shortcuts are window-wide except for plain Ctrl+Z in a detail
    // EDIT, which the control keeps for its own text undo (see
    // HandleEditorKey). Routing before DispatchMessage keeps the shortcut from
    // a focused child that would otherwise consume it; every other key still
    // reaches its control.
    if (message.message == WM_KEYDOWN) {
      const bool focus_in_edit = FocusIsEdit();
      const bool ctrl = GetKeyState(VK_CONTROL) < 0;
      const bool shift = GetKeyState(VK_SHIFT) < 0;
      if (HandleEditorKey(focus_in_edit, ctrl, shift,
                          static_cast<int>(message.wParam))) {
        continue;
      }
    }
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
    case WM_ERASEBKGND: {
      // The theme is per window, so the shared class brush cannot carry it:
      // erase here from the resolved scheme instead of letting DefWindowProc
      // paint the class's default light brush.
      auto* dc = reinterpret_cast<HDC>(wparam);
      RECT client{};
      if (background_brush_ == nullptr || dc == nullptr ||
          GetClientRect(hwnd, &client) == 0) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
      }
      FillRect(dc, &client, background_brush_);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      // The status line, the detail labels/edits, the assets drop-down list,
      // and the pane's check box all paint on the themed surface with the
      // themed text colour. A disabled EDIT arrives as WM_CTLCOLORSTATIC.
      auto* dc = reinterpret_cast<HDC>(wparam);
      if (dc != nullptr) {
        SetTextColor(dc, colors_.text);
        SetBkColor(dc, colors_.background);
      }
      return reinterpret_cast<LRESULT>(background_brush_);
    }
    case WM_DPICHANGED: {
      // The suggested rect in lparam is already in the new DPI's physical
      // pixels; apply it and re-scale every layout metric to the new DPI.
      const UINT new_dpi = HIWORD(wparam);
      if (new_dpi != 0) dpi_ = new_dpi;
      // Recreate and re-apply the font at the new DPI before laying out, so the
      // scaled controls never render their old, clipped text. The theme is
      // re-applied too: its brushes are DPI-independent, but the apply is cheap
      // and keeps every themed surface in step after the move.
      CreateFontForDpi();
      ApplyFont();
      ApplyTheme();
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested != nullptr) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      RECT client{};
      if (GetClientRect(hwnd, &client) != 0) {
        Layout(client.right - client.left, client.bottom - client.top);
      }
      return 0;
    }
    case WM_GETMINMAXINFO: {
      // DefWindowProc fills the maximize defaults; only raise the minimum
      // tracking size so the toolbar, the pane's Apply button, and the status
      // line always fit while the user drags a border.
      DefWindowProcW(hwnd, message, wparam, lparam);
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      if (info == nullptr) return 0;
      RECT minimum{0, 0, Scale(kMinClientWidth), Scale(kMinClientHeight)};
      const DWORD style =
          static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
      const DWORD ex_style =
          static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      if (AdjustWindowRectExForDpi(&minimum, style, FALSE, ex_style, dpi_) != 0) {
        info->ptMinTrackSize.x = minimum.right - minimum.left;
        info->ptMinTrackSize.y = minimum.bottom - minimum.top;
      }
      return 0;
    }
    case WM_DROPFILES: {
      // DragFinish releases the shell's block exactly once on every path
      // through the handler, including both rejections.
      const HDROP drop = reinterpret_cast<HDROP>(wparam);
      HandleDroppedFiles(drop);
      DragFinish(drop);
      return 0;
    }
    case WM_CLOSE:
      OnClose();
      return 0;
    case WM_TIMER: {
      // Only the running liveness poll is dispatched; any other timer id (or a
      // tick after the poll stopped) is not the watchdog's.
      if (wparam == kWatchdogPollTimerId && watchdog_poll_running_ &&
          watchdog_poll_handler_) {
        watchdog_poll_handler_();
      }
      return 0;
    }
    case kConfigChangedMessage:
      // Posted by the config-file watcher; the app decides whether to reload or
      // report a conflict.
      if (config_changed_handler_) config_changed_handler_();
      return 0;
    case WM_KEYDOWN: {
      // GetKeyState reads the thread's key state, which is why the routing
      // itself is a helper: a test can exercise it without physical input.
      const bool focus_in_edit = FocusIsEdit();
      const bool ctrl = GetKeyState(VK_CONTROL) < 0;
      const bool shift = GetKeyState(VK_SHIFT) < 0;
      if (HandleEditorKey(focus_in_edit, ctrl, shift,
                          static_cast<int>(wparam))) {
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_COMMAND: {
      if (HIWORD(wparam) != BN_CLICKED) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
      }
      switch (LOWORD(wparam)) {
        case kApplyButtonId:
          ApplyDetail();
          return 0;
        case kAddButtonId:
          AddTarget();
          return 0;
        case kRemoveButtonId:
          RemoveSelectedTarget();
          return 0;
        case kSaveButtonId:
          SaveConfig();
          return 0;
        case kValidateButtonId:
          ValidateConfig();
          return 0;
        case kStartButtonId:
          StartWatchdog();
          return 0;
        case kStopButtonId:
          StopWatchdog();
          return 0;
        default:
          return DefWindowProcW(hwnd, message, wparam, lparam);
      }
    }
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
      if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
      }
      if (background_brush_ != nullptr) {
        DeleteObject(background_brush_);
        background_brush_ = nullptr;
        brush_color_ = CLR_INVALID;
      }
      KillTimer(hwnd, kWatchdogPollTimerId);
      watchdog_poll_running_ = false;
      handle_ = nullptr;
      list_view_ = nullptr;
      status_view_ = nullptr;
      add_button_ = nullptr;
      remove_button_ = nullptr;
      save_button_ = nullptr;
      validate_button_ = nullptr;
      start_button_ = nullptr;
      stop_button_ = nullptr;
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
