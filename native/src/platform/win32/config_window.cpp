#include "platform/win32/config_window.h"

#include <windows.h>

#include <commctrl.h>

namespace htmlppt {
namespace {

constexpr wchar_t kWindowClass[] = L"HtmlpptConfigWindow";

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
  Layout();
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
}

void ConfigWindow::Layout() {
  if (handle_ == nullptr || list_view_ == nullptr) return;
  RECT client{};
  GetClientRect(handle_, &client);
  constexpr int kStatusHeight = 24;
  const int height = client.bottom - client.top - kStatusHeight;
  MoveWindow(list_view_, client.left, client.top, client.right - client.left,
             height > 0 ? height : 0, TRUE);
  MoveWindow(status_view_, client.left, client.bottom - kStatusHeight,
             client.right - client.left, kStatusHeight, TRUE);
}

void ConfigWindow::SetRows(const std::vector<DisplayRow>& rows) {
  if (list_view_ == nullptr) return;
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
    item.lParam = static_cast<LPARAM>(i);
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
}

void ConfigWindow::SetStatus(const std::wstring& text) {
  if (status_view_ == nullptr) return;
  SetWindowTextW(status_view_, text.c_str());
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
      Layout();
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      // Child windows are gone with the parent; never hold stale HWNDs.
      handle_ = nullptr;
      list_view_ = nullptr;
      status_view_ = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

} // namespace htmlppt
