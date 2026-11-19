#include "platform/win32/wizard_window.h"

#include <windows.h>

#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>
#include <optional>
#include <system_error>
#include <utility>

#include "core/utf8_path.h"
#include "platform/win32/utf8_win32.h"

namespace htmlppt {
namespace {

constexpr wchar_t kWizardClass[] = L"HtmlpptWizardWindow";

// Stable ids so each control's BN_CLICKED can be routed in WM_COMMAND.
constexpr int kChooseFolderButtonId = 2001;
constexpr int kFilenameRadioId = 2002;
constexpr int kOrderRadioId = 2003;
constexpr int kImportButtonId = 2004;
constexpr int kCancelButtonId = 2005;

// Layout in 96-DPI logical units: a folder row on top, the preview table, the
// status line, and the Import/Cancel row at the bottom.
constexpr int kMargin = 12;
constexpr int kRowHeight = 28;
constexpr int kStatusHeight = 24;
constexpr int kFolderButtonWidth = 170;
constexpr int kRadioWidth = 120;
constexpr int kActionButtonWidth = 100;
constexpr int kActionButtonGap = 8;
constexpr int kMinClientWidth = 660;
constexpr int kMinClientHeight = 420;

std::wstring Widen(const std::string& utf8) {
  const std::optional<std::wstring> wide = Utf8ToWide(utf8);
  return wide.value_or(std::wstring{});
}

// Seeds the shell folder picker with the wizard's current folder. The pointer
// is the wizard's live initial path, which outlives the modal dialog call.
int CALLBACK BrowseCallback(HWND hwnd, UINT message, LPARAM lparam,
                            LPARAM data) {
  (void)lparam;
  if (message == BFFM_INITIALIZED && data != 0) {
    const auto* initial = reinterpret_cast<const std::wstring*>(data);
    if (!initial->empty()) {
      SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE,
                   reinterpret_cast<LPARAM>(initial->c_str()));
    }
  }
  return 0;
}

} // namespace

WizardWindow::~WizardWindow() {
  if (handle_ != nullptr) {
    DestroyWindow(handle_);
    handle_ = nullptr;
  }
  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
}

bool WizardWindow::Create(const WizardWindowOptions& options) {
  if (handle_ != nullptr) return false;

  // Common Controls v6 is initialized once per process; the list view class
  // must be available before the control is created.
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
    wc.lpfnWndProc = &WizardWindow::WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // IDC_ARROW expands to a narrow resource name without UNICODE defined.
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWizardClass;
    window_class = RegisterClassExW(&wc);
    if (window_class == 0) return false;
  }

  owner_ = options.owner;
  slides_ = options.slides;
  html_folder_ = options.html_folder;

  handle_ = CreateWindowExW(0, kWizardClass, options.title.c_str(),
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            options.width, options.height, owner_, nullptr,
                            GetModuleHandleW(nullptr), this);
  if (handle_ == nullptr) return false;
  dpi_ = GetDpiForWindow(handle_);
  if (dpi_ == 0) dpi_ = 96;
  // The options are 96-DPI logical units; scale the initial size to the DPI of
  // the monitor the window opened on before laying out the children.
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

  // The seeded folder is scanned on open, so the wizard arrives with its
  // proposals ready; an empty seed waits for "Choose HTML folder...".
  if (!html_folder_.empty()) {
    ScanFolder(html_folder_);
  } else {
    FillList();
  }
  if (options.show) {
    ShowWindow(handle_, SW_SHOW);
    UpdateWindow(handle_);
  }
  return true;
}

void WizardWindow::CreateChildren() {
  CreateFontForDpi();
  const auto make_button = [&](const wchar_t* text, int id, DWORD style) {
    return CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0,
        0, handle_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
  };
  choose_folder_button_ = make_button(L"Choose HTML folder...",
                                      kChooseFolderButtonId, BS_PUSHBUTTON);
  filename_radio_ = make_button(L"By file name", kFilenameRadioId,
                                BS_AUTORADIOBUTTON | WS_GROUP);
  order_radio_ = make_button(L"By file order", kOrderRadioId,
                             BS_AUTORADIOBUTTON);
  import_button_ =
      make_button(L"Import", kImportButtonId, BS_PUSHBUTTON | BS_DEFPUSHBUTTON);
  cancel_button_ = make_button(L"Cancel", kCancelButtonId, BS_PUSHBUTTON);
  SendMessageW(filename_radio_, BM_SETCHECK, BST_CHECKED, 0);

  list_view_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
      0, 0, 0, 0, handle_, nullptr, GetModuleHandleW(nullptr), nullptr);
  ListView_SetExtendedListViewStyle(list_view_,
                                    LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
  const wchar_t* headings[] = {L"SLIDE", L"TITLE", L"HTML"};
  const int widths[] = {60, 220, 400};
  for (int index = 0; index < 3; ++index) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(headings[index]);
    column.cx = Scale(widths[index]);
    column.iSubItem = index;
    ListView_InsertColumn(list_view_, index, &column);
  }

  status_view_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 0, 0, 0, 0, handle_, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
  ApplyFont();
  // An empty seed has no proposals yet; the hint tells the user the first move.
  FillList();
}

void WizardWindow::CreateFontForDpi() {
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

void WizardWindow::ApplyFont() {
  if (font_ == nullptr) return;
  HWND children[] = {choose_folder_button_, filename_radio_, order_radio_,
                     import_button_,        cancel_button_,  list_view_,
                     status_view_};
  for (HWND child : children) {
    if (child != nullptr) {
      SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
  }
}

int WizardWindow::Scale(int value) const {
  return MulDiv(value, static_cast<int>(dpi_), 96);
}

void WizardWindow::Layout(int width, int height) {
  if (handle_ == nullptr || list_view_ == nullptr || status_view_ == nullptr) {
    return;
  }
  const int margin = Scale(kMargin);
  const int row_height = Scale(kRowHeight);
  const int top = margin;
  int left = margin;
  if (choose_folder_button_ != nullptr) {
    MoveWindow(choose_folder_button_, left, top, Scale(kFolderButtonWidth),
               row_height, TRUE);
  }
  left += Scale(kFolderButtonWidth) + Scale(kActionButtonGap) * 2;
  if (filename_radio_ != nullptr) {
    MoveWindow(filename_radio_, left, top, Scale(kRadioWidth), row_height, TRUE);
  }
  left += Scale(kRadioWidth) + Scale(kActionButtonGap);
  if (order_radio_ != nullptr) {
    MoveWindow(order_radio_, left, top, Scale(kRadioWidth), row_height, TRUE);
  }

  const int action_width = Scale(kActionButtonWidth);
  const int actions_top = height - margin - row_height;
  const int status_height = Scale(kStatusHeight);
  const int status_top = actions_top - Scale(kActionButtonGap) - status_height;
  MoveWindow(status_view_, margin, status_top, width - 2 * margin,
             status_height, TRUE);
  if (cancel_button_ != nullptr) {
    MoveWindow(cancel_button_, width - margin - action_width, actions_top,
               action_width, row_height, TRUE);
  }
  if (import_button_ != nullptr) {
    MoveWindow(import_button_,
               width - margin - action_width - Scale(kActionButtonGap) -
                   action_width,
               actions_top, action_width, row_height, TRUE);
  }

  const int list_top = top + row_height + Scale(kActionButtonGap);
  const int list_height = status_top - Scale(kActionButtonGap) - list_top;
  MoveWindow(list_view_, margin, list_top, width - 2 * margin,
             list_height > 0 ? list_height : 0, TRUE);
  if (width > 2 * margin) {
    const int html_width = width - 2 * margin - Scale(60 + 220);
    ListView_SetColumnWidth(list_view_, 2,
                            html_width > Scale(120) ? html_width : Scale(120));
  }
}

void WizardWindow::ScanFolder(const std::filesystem::path& folder) {
  html_folder_ = folder;
  html_files_.clear();
  std::error_code error;
  std::filesystem::directory_iterator iterator(folder, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    std::error_code file_error;
    if (std::filesystem::is_regular_file(iterator->path(), file_error) &&
        !file_error) {
      html_files_.push_back(Utf8Path(iterator->path()));
    }
    iterator.increment(error);
  }
  // A stable order makes "first file wins" deterministic and is also what the
  // ByFileOrder strategy zips against.
  std::sort(html_files_.begin(), html_files_.end());
  RecomputePairs();
}

void WizardWindow::RecomputePairs() {
  pairs_ = ProposePairs(slides_, html_files_, mode_);
  FillList();
}

void WizardWindow::FillList() {
  if (list_view_ == nullptr) return;
  SendMessageW(list_view_, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list_view_);
  for (std::size_t index = 0; index < pairs_.size(); ++index) {
    const WizardPair& pair = pairs_[index];
    const std::wstring slide = std::to_wstring(pair.slide);
    LVITEMW item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = static_cast<int>(index);
    item.iSubItem = 0;
    item.pszText = const_cast<wchar_t*>(slide.c_str());
    // The row's position in pairs_: checked_pairs() reads the list's check
    // state and maps it back through this value.
    item.lParam = static_cast<LPARAM>(index);
    const int inserted = ListView_InsertItem(list_view_, &item);
    if (inserted < 0) continue;
    const auto set_text = [&](int column, const std::wstring& text) {
      ListView_SetItemText(list_view_, inserted, column,
                           const_cast<wchar_t*>(text.c_str()));
    };
    set_text(1, Widen(pair.title));
    set_text(2, Widen(pair.html));
    ListView_SetCheckState(list_view_, inserted, pair.included ? TRUE : FALSE);
  }
  SendMessageW(list_view_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(list_view_, nullptr, TRUE);

  if (pairs_.empty()) {
    SetStatus(html_folder_.empty()
                  ? L"choose an HTML folder to scan for .html files"
                  : L"no matching HTML files in " + html_folder_.wstring());
    return;
  }
  std::vector<std::string> htmls;
  htmls.reserve(pairs_.size());
  for (const WizardPair& pair : pairs_) htmls.push_back(pair.html);
  std::wstring text = std::to_wstring(pairs_.size()) + L" of " +
                      std::to_wstring(slides_.size()) + L" slides paired";
  const std::string root = SuggestHtmlRoot(htmls);
  if (!root.empty()) text += L" (" + Widen(root) + L")";
  SetStatus(text);
}

std::vector<WizardPair> WizardWindow::checked_pairs() const {
  std::vector<WizardPair> checked;
  if (list_view_ == nullptr) return checked;
  const int count = ListView_GetItemCount(list_view_);
  for (int row = 0; row < count && row < static_cast<int>(pairs_.size());
       ++row) {
    if (ListView_GetCheckState(list_view_, row) == 0) continue;
    WizardPair pair = pairs_[static_cast<std::size_t>(row)];
    pair.included = true;
    checked.push_back(std::move(pair));
  }
  return checked;
}

void WizardWindow::SetPairsForTest(const std::vector<WizardPair>& pairs) {
  pairs_ = pairs;
  FillList();
}

void WizardWindow::SetStatus(const std::wstring& text) {
  if (status_view_ == nullptr) return;
  SetWindowTextW(status_view_, text.c_str());
}

void WizardWindow::ChooseFolder() {
  // The picker is modal and manual: it asks the shell for one folder and scans
  // it. The initial folder is the wizard's current root, which the callback
  // selects once the dialog exists.
  std::wstring initial = html_folder_.wstring();
  BROWSEINFOW info{};
  info.hwndOwner = handle_;
  info.lpszTitle = L"Choose the folder that holds the slide HTML files";
  info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
  info.lpfn = &BrowseCallback;
  info.lParam = reinterpret_cast<LPARAM>(&initial);
  LPITEMIDLIST item = SHBrowseForFolderW(&info);
  if (item == nullptr) return;  // the user cancelled
  wchar_t path[MAX_PATH] = {};
  const BOOL copied = SHGetPathFromIDListW(item, path);
  CoTaskMemFree(item);
  if (copied == FALSE || path[0] == L'\0') return;
  ScanFolder(std::filesystem::path(path));
}

void WizardWindow::Import() {
  std::vector<WizardPair> checked = checked_pairs();
  if (checked.empty()) {
    // An empty import would be a silent no-op; say why the window stays open.
    SetStatus(L"check at least one row to import");
    return;
  }
  result_.accepted = true;
  result_.pairs = std::move(checked);
  result_.html_folder = html_folder_;
  done_ = true;
  if (handle_ != nullptr) DestroyWindow(handle_);
}

void WizardWindow::Cancel() {
  result_.accepted = false;
  result_.pairs.clear();
  result_.html_folder = html_folder_;
  done_ = true;
  if (handle_ != nullptr) DestroyWindow(handle_);
}

bool WizardWindow::Show() {
  if (handle_ == nullptr) return false;
  if (owner_ != nullptr) EnableWindow(owner_, FALSE);
  ShowWindow(handle_, SW_SHOW);
  UpdateWindow(handle_);
  MSG message;
  while (!done_) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == -1) break;
    if (result == 0) {
      // WM_QUIT is not ours to swallow; hand it back to the outer loop.
      PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  // Import and Cancel destroy the window themselves; a loop exit through an
  // error or a foreign WM_QUIT still leaves nothing behind.
  if (handle_ != nullptr) DestroyWindow(handle_);
  if (owner_ != nullptr) {
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
  }
  return result_.accepted;
}

bool WizardWindow::Close() {
  if (handle_ == nullptr) return false;
  return PostMessageW(handle_, WM_CLOSE, 0, 0) != FALSE;
}

LRESULT CALLBACK WizardWindow::WindowProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<WizardWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) return self->HandleMessage(hwnd, message, wparam, lparam);
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT WizardWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      Layout(static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam)));
      return 0;
    case WM_DPICHANGED: {
      const UINT new_dpi = HIWORD(wparam);
      if (new_dpi != 0) dpi_ = new_dpi;
      CreateFontForDpi();
      ApplyFont();
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
    case WM_CLOSE:
      Cancel();
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Cancel();
        return 0;
      }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_COMMAND: {
      if (HIWORD(wparam) != BN_CLICKED) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
      }
      switch (LOWORD(wparam)) {
        case kChooseFolderButtonId:
          ChooseFolder();
          return 0;
        case kFilenameRadioId:
          mode_ = PairingMode::ByFileName;
          RecomputePairs();
          return 0;
        case kOrderRadioId:
          mode_ = PairingMode::ByFileOrder;
          RecomputePairs();
          return 0;
        case kImportButtonId:
          Import();
          return 0;
        case kCancelButtonId:
          Cancel();
          return 0;
        default:
          return DefWindowProcW(hwnd, message, wparam, lparam);
      }
    }
    case WM_DESTROY:
      // A modal wizard runs a nested loop: this window must not post WM_QUIT,
      // which belongs to the editor's own loop.
      if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
      }
      handle_ = nullptr;
      list_view_ = nullptr;
      status_view_ = nullptr;
      choose_folder_button_ = nullptr;
      filename_radio_ = nullptr;
      order_radio_ = nullptr;
      import_button_ = nullptr;
      cancel_button_ = nullptr;
      done_ = true;
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

} // namespace htmlppt
