#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <optional>
#include <string>

#include "platform/win32/config_window.h"

HTMLPPT_TEST_CASE(win32_config_window_creates_and_closes) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.title = L"htmlppt config";
  options.show = false;  // never flash a window in a test run
  HTMLPPT_CHECK(window.Create(options));
  HTMLPPT_CHECK(window.handle() != nullptr);
  HTMLPPT_CHECK(IsWindow(window.handle()) != 0);

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}
#endif

#include <commctrl.h>

#include "core/config_editor.h"

namespace {

// Scales one 96-DPI layout unit to `dpi`.
int Scaled(int value, UINT dpi) {
  return MulDiv(value, static_cast<int>(dpi), 96);
}

// Reads the font a control actually uses; 0 means WM_SETFONT never ran.
int FontHeight(HWND control) {
  if (control == nullptr) return 0;
  const HFONT font =
      reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
  if (font == nullptr) return 0;
  LOGFONTW logfont{};
  if (GetObjectW(font, sizeof(logfont), &logfont) == 0) return 0;
  return logfont.lfHeight;
}

// Drains the message queue until a posted WM_CLOSE has destroyed the window.
void PumpUntilDestroyed(HWND hwnd) {
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(hwnd) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
}

// A unique path under the user's temp directory. The caller creates and removes
// the file or directory it names.
std::wstring UniqueTempPath(const wchar_t* suffix) {
  wchar_t directory[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, directory);
  if (length == 0 || length >= MAX_PATH) return {};
  static unsigned counter = 0;
  ++counter;
  return std::wstring(directory) + L"htmlppt-b3-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter) + suffix;
}

// Builds the block WM_DROPFILES carries: one DROPFILES header with fWide = TRUE
// followed by a double-null-terminated UTF-16 path list. GMEM_MOVEABLE is what
// DragFinish expects to free; GMEM_ZEROINIT supplies the list's two terminators.
HGLOBAL MakeDropFiles(const std::wstring& path) {
  const SIZE_T size = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
  HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
  if (block == nullptr) return nullptr;
  auto* data = static_cast<BYTE*>(GlobalLock(block));
  if (data == nullptr) {
    GlobalFree(block);
    return nullptr;
  }
  auto* header = reinterpret_cast<DROPFILES*>(data);
  header->pFiles = sizeof(DROPFILES);
  header->fWide = TRUE;
  std::memcpy(data + sizeof(DROPFILES), path.c_str(),
              path.size() * sizeof(wchar_t));
  GlobalUnlock(block);
  return block;
}

// Sends one synthetic WM_DROPFILES. The window's handler owns the block and
// frees it through DragFinish, so the test never frees it: an invalid handle
// proves the handler freed it exactly once (a leak would leave it valid, and a
// second free would corrupt the heap instead).
void SendDrop(HWND hwnd, const std::wstring& path) {
  HGLOBAL block = MakeDropFiles(path);
  HTMLPPT_CHECK(block != nullptr);
  SendMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(block), 0);
  HTMLPPT_CHECK_EQ(GlobalFlags(block),
                   static_cast<UINT>(GMEM_INVALID_HANDLE));
}

// Reads the status line into a fixed buffer.
std::wstring StatusText(HWND status_view) {
  wchar_t status[128] = {};
  GetWindowTextW(status_view, status, 128);
  return status;
}

// DWMWA_USE_IMMERSIVE_DARK_MODE as documented (20) and the pre-20H1 number.
constexpr DWORD kDarkModeAttribute = 20;
constexpr DWORD kDarkModeAttributeLegacy = 19;

// The immersive dark-mode title bar attribute; nullopt when this Windows build
// has no such attribute, in which case the title-bar assertion is skipped.
std::optional<bool> QueryDarkTitleBar(HWND hwnd) {
  BOOL value = FALSE;
  HRESULT result =
      DwmGetWindowAttribute(hwnd, kDarkModeAttribute, &value, sizeof value);
  if (FAILED(result)) {
    result = DwmGetWindowAttribute(hwnd, kDarkModeAttributeLegacy, &value,
                                   sizeof value);
  }
  if (FAILED(result)) return std::nullopt;
  return value != FALSE;
}

// The current user's AppsUseLightTheme value, read the same way the window's
// System theme resolves it. Missing/unreadable folds into nullopt (light).
std::optional<DWORD> ReadAppsUseLightThemeForTest() {
  HKEY key = nullptr;
  constexpr wchar_t kPersonalize[] =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
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

// Reads the solid colour of the brush a WM_CTLCOLOR* handler returned.
std::optional<COLORREF> BrushColor(HBRUSH brush) {
  if (brush == nullptr) return std::nullopt;
  LOGBRUSH logbrush{};
  if (GetObjectW(brush, sizeof(logbrush), &logbrush) == 0) {
    return std::nullopt;
  }
  return logbrush.lbColor;
}

// The single client-sized pixel a WM_ERASEBKGND handler painted at (1, 1).
std::optional<COLORREF> ErasedBackgroundColor(HWND hwnd) {
  RECT client{};
  if (GetClientRect(hwnd, &client) == 0 || client.right <= 1 ||
      client.bottom <= 1) {
    return std::nullopt;
  }
  HDC window_dc = GetDC(hwnd);
  if (window_dc == nullptr) return std::nullopt;
  HDC memory_dc = CreateCompatibleDC(window_dc);
  HBITMAP canvas = nullptr;
  if (memory_dc != nullptr) {
    canvas = CreateCompatibleBitmap(window_dc, client.right, client.bottom);
  }
  if (memory_dc == nullptr || canvas == nullptr) {
    if (memory_dc != nullptr) DeleteDC(memory_dc);
    ReleaseDC(hwnd, window_dc);
    return std::nullopt;
  }
  const HGDIOBJ previous = SelectObject(memory_dc, canvas);
  const LRESULT erased = SendMessageW(
      hwnd, WM_ERASEBKGND, reinterpret_cast<WPARAM>(memory_dc), 0);
  std::optional<COLORREF> color;
  if (erased != 0) color = GetPixel(memory_dc, 1, 1);
  SelectObject(memory_dc, previous);
  DeleteObject(canvas);
  DeleteDC(memory_dc);
  ReleaseDC(hwnd, window_dc);
  return color;
}

// Sends one WM_CTLCOLOR* to the window and reads the returned brush's colour;
// `text_color`, when given, receives the text colour the handler set on the DC.
std::optional<COLORREF> ControlSurfaceColor(HWND hwnd, UINT message,
                                            HWND control,
                                            COLORREF* text_color = nullptr) {
  HDC dc = GetDC(hwnd);
  if (dc == nullptr) return std::nullopt;
  const LRESULT result =
      SendMessageW(hwnd, message, reinterpret_cast<WPARAM>(dc),
                   reinterpret_cast<LPARAM>(control));
  if (text_color != nullptr) *text_color = GetTextColor(dc);
  const std::optional<COLORREF> color =
      BrushColor(reinterpret_cast<HBRUSH>(result));
  ReleaseDC(hwnd, dc);
  return color;
}

} // namespace

HTMLPPT_TEST_CASE(win32_config_window_shows_the_target_rows) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  HTMLPPT_CHECK(window.list_view() != nullptr);

  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].index = 0;
  rows[0].slide = 3;
  rows[0].label = "Live";
  rows[0].html = "page.html";
  rows[0].assets_mode = "file";
  rows[0].state = "resolved";
  rows[1].index = 1;
  rows[1].slide = 5;
  rows[1].html = "two.html";
  rows[1].state = "error";
  rows[1].issue = "slide 5 is already mapped by target 1";
  window.SetRows(rows);

  HTMLPPT_CHECK_EQ(ListView_GetItemCount(window.list_view()), 2);
  wchar_t text[128] = {};
  ListView_GetItemText(window.list_view(), 0, 0, text, 128);
  HTMLPPT_CHECK_EQ(std::wstring(text), std::wstring(L"3"));
  ListView_GetItemText(window.list_view(), 1, 4, text, 128);
  // compare(0, 5, ...) is the same "starts with error" check as
  // wstring(text).rfind(L"error", 0) == 0; GCC 16 mis-folds the temporary
  // string built for rfind under -O3/-Werror.
  HTMLPPT_CHECK(std::wstring(text).compare(0, 5, L"error") == 0);

  window.SetStatus(L"2 target(s)");
  wchar_t status[64] = {};
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"2 target(s)"));

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
  HTMLPPT_CHECK(window.list_view() == nullptr);
  HTMLPPT_CHECK(window.status_view() == nullptr);
}

HTMLPPT_TEST_CASE(win32_config_window_binds_selection_to_the_detail_pane) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].index = 0; rows[0].slide = 3; rows[0].label = "Live";
  rows[0].html = "page.html"; rows[0].assets_mode = "file";
  rows[0].state = "error"; rows[0].issue = "slide 3 is already mapped";
  rows[0].enabled = false; rows[0].assets_explicit = false;
  rows[1].index = 1; rows[1].slide = 5; rows[1].html = "two.html";
  rows[1].state = "resolved"; rows[1].assets_mode = "tree";
  rows[1].assets_explicit = true;
  window.SetRows(rows);

  HWND enabled_check =
      FindWindowExW(window.handle(), nullptr, L"BUTTON", L"Enabled");
  HWND assets_combo =
      FindWindowExW(window.handle(), nullptr, L"COMBOBOX", nullptr);
  HTMLPPT_CHECK(enabled_check != nullptr);
  HTMLPPT_CHECK(assets_combo != nullptr);

  // A disabled target with an error stays disabled and keeps the error in the
  // STATE column.
  wchar_t state_text[128] = {};
  ListView_GetItemText(window.list_view(), 0, 4, state_text, 128);
  // compare(0, 5, L"error") is the same "starts with error" check as
  // wstring(state_text).rfind(L"error", 0) == 0, without a temporary string
  // built from the literal (GCC 16 mis-folds that under -O3/-Werror).
  HTMLPPT_CHECK(std::wstring(state_text).compare(0, 5, L"error") == 0);

  window.SelectRow(0);
  auto fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"3"));
  HTMLPPT_CHECK_EQ(fields.html, std::wstring(L"page.html"));
  HTMLPPT_CHECK(!fields.enabled);
  HTMLPPT_CHECK_EQ(SendMessageW(enabled_check, BM_GETCHECK, 0, 0),
                   BST_UNCHECKED);
  // Inherited assets show "inherit", not the effective stage mode.
  int selection =
      static_cast<int>(SendMessageW(assets_combo, CB_GETCURSEL, 0, 0));
  HTMLPPT_CHECK_EQ(selection, 0);
  wchar_t asset_text[32] = {};
  SendMessageW(assets_combo, CB_GETLBTEXT, static_cast<WPARAM>(selection),
               reinterpret_cast<LPARAM>(asset_text));
  HTMLPPT_CHECK_EQ(std::wstring(asset_text), std::wstring(L"inherit"));

  window.SelectRow(1);
  fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"5"));
  HTMLPPT_CHECK(fields.enabled);
  HTMLPPT_CHECK_EQ(SendMessageW(enabled_check, BM_GETCHECK, 0, 0), BST_CHECKED);
  // An explicit assets block shows its effective mode.
  selection = static_cast<int>(SendMessageW(assets_combo, CB_GETCURSEL, 0, 0));
  HTMLPPT_CHECK_EQ(selection, 3);
  SendMessageW(assets_combo, CB_GETLBTEXT, static_cast<WPARAM>(selection),
               reinterpret_cast<LPARAM>(asset_text));
  HTMLPPT_CHECK_EQ(std::wstring(asset_text), std::wstring(L"tree"));

  // A cleared selection disables the pane instead of writing to a stale index.
  window.SelectRow(-1);
  HTMLPPT_CHECK(!window.detail_enabled());
}

HTMLPPT_TEST_CASE(win32_config_window_disables_save_while_a_row_is_an_error) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  window.SetSaveEnabled(false);
  HTMLPPT_CHECK(!window.save_enabled());
  window.SetSaveEnabled(true);
  HTMLPPT_CHECK(window.save_enabled());
}

HTMLPPT_TEST_CASE(win32_config_window_save_button_follows_the_gate) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  HTMLPPT_CHECK(window.save_button() != nullptr);
  // Create() starts with a closed gate: the model is not dirty yet.
  HTMLPPT_CHECK(!window.save_enabled());
  HTMLPPT_CHECK(IsWindowEnabled(window.save_button()) == 0);
  window.SetSaveEnabled(true);
  HTMLPPT_CHECK(IsWindowEnabled(window.save_button()) != 0);
  window.SetSaveEnabled(false);
  HTMLPPT_CHECK(IsWindowEnabled(window.save_button()) == 0);

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_forwards_the_toolbar_buttons) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].index = 0; rows[0].slide = 3; rows[0].html = "page.html";
  rows[0].state = "resolved";
  rows[1].index = 7; rows[1].slide = 5; rows[1].html = "two.html";
  rows[1].state = "resolved";
  window.SetRows(rows);

  // Clicking a button is the same WM_COMMAND/BN_CLICKED route a real click uses.
  const auto click = [&](HWND button) {
    SendMessageW(window.handle(), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED),
                 reinterpret_cast<LPARAM>(button));
  };

  int adds = 0;
  window.SetAddHandler([&adds](std::wstring* error) {
    ++adds;
    if (error != nullptr) error->clear();
    return true;
  });
  HTMLPPT_CHECK(window.add_button() != nullptr);
  click(window.add_button());
  HTMLPPT_CHECK_EQ(adds, 1);

  // Remove forwards the selected row's model index, not its list position.
  window.SelectRow(1);
  int removes = 0;
  std::size_t removed = 0;
  window.SetRemoveConfirmHandler([] { return true; });
  window.SetRemoveHandler([&](std::size_t index, std::wstring* error) {
    ++removes;
    removed = index;
    if (error != nullptr) error->clear();
    return true;
  });
  click(window.remove_button());
  HTMLPPT_CHECK_EQ(removes, 1);
  HTMLPPT_CHECK_EQ(removed, std::size_t{7});

  // A refused confirmation never reaches the model.
  window.SetRemoveConfirmHandler([] { return false; });
  click(window.remove_button());
  HTMLPPT_CHECK_EQ(removes, 1);

  // Without a selection the view reports instead of removing an arbitrary row.
  window.SetRemoveConfirmHandler([] { return true; });
  window.SelectRow(-1);
  click(window.remove_button());
  HTMLPPT_CHECK_EQ(removes, 1);
  wchar_t status[64] = {};
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"no row selected"));

  // Save reports the handler's outcome.
  window.SetSaveHandler([](std::wstring* error) {
    if (error != nullptr) error->clear();
    return true;
  });
  click(window.save_button());
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"saved"));

  window.SetSaveHandler([](std::wstring* error) {
    if (error != nullptr) *error = L"cannot write watcher.json";
    return false;
  });
  click(window.save_button());
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status),
                   std::wstring(L"cannot write watcher.json"));

  // Validate surfaces the first blocking issue the handler reports (including
  // a global one that has no row of its own).
  int validates = 0;
  window.SetValidateHandler([&validates](std::wstring* error) {
    ++validates;
    if (error != nullptr) *error = L"deck must not be empty";
    return false;
  });
  click(window.validate_button());
  HTMLPPT_CHECK_EQ(validates, 1);
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status),
                   std::wstring(L"deck must not be empty"));

  // Removing the handlers turns the same clicks into no-ops, not crashes.
  window.SetAddHandler(nullptr);
  window.SetRemoveHandler(nullptr);
  window.SetSaveHandler(nullptr);
  window.SetValidateHandler(nullptr);
  click(window.add_button());
  click(window.remove_button());
  click(window.save_button());
  click(window.validate_button());
  HTMLPPT_CHECK_EQ(adds, 1);
  HTMLPPT_CHECK_EQ(removes, 1);
  HTMLPPT_CHECK_EQ(validates, 1);

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_apply_forwards_the_detail_fields) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(1);
  rows[0].index = 0; rows[0].slide = 3; rows[0].label = "Live";
  rows[0].html = "page.html"; rows[0].state = "resolved";
  window.SetRows(rows);
  window.SelectRow(0);

  htmlppt::DetailFields captured;
  int calls = 0;
  // The installed handler owns the success status: it is the one that knows
  // whether the refresh left a blocking error, so it writes "applied" itself.
  window.SetEditHandler(
      [&](const htmlppt::DetailFields& fields, std::wstring* error) {
        captured = fields;
        ++calls;
        if (error != nullptr) error->clear();
        window.SetStatus(L"applied");
        return true;
      });
  HTMLPPT_CHECK(window.apply_button() != nullptr);
  const int apply_id = GetDlgCtrlID(window.apply_button());
  const auto click_apply = [&]() {
    SendMessageW(window.handle(), WM_COMMAND,
                 MAKEWPARAM(apply_id, BN_CLICKED),
                 reinterpret_cast<LPARAM>(window.apply_button()));
  };
  click_apply();
  HTMLPPT_CHECK_EQ(calls, 1);
  HTMLPPT_CHECK_EQ(captured.slide, std::wstring(L"3"));
  HTMLPPT_CHECK_EQ(captured.html, std::wstring(L"page.html"));
  HTMLPPT_CHECK_EQ(captured.label, std::wstring(L"Live"));
  wchar_t status[64] = {};
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"applied"));

  // A blocking reason reported by the handler must survive the click: the view
  // must not clobber it with a blanket "applied".
  window.SetEditHandler(
      [&](const htmlppt::DetailFields&, std::wstring* error) {
        ++calls;
        if (error != nullptr) error->clear();
        window.SetStatus(L"applied - save blocked by: deck must not be empty");
        return true;
      });
  click_apply();
  HTMLPPT_CHECK_EQ(calls, 2);
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(
      std::wstring(status),
      std::wstring(L"applied - save blocked by: deck must not be empty"));

  // A handler that writes no success status leaves the previous text alone;
  // the view adds nothing of its own.
  window.SetStatus(L"previous");
  window.SetEditHandler(
      [&](const htmlppt::DetailFields&, std::wstring* error) {
        ++calls;
        if (error != nullptr) error->clear();
        return true;
      });
  click_apply();
  HTMLPPT_CHECK_EQ(calls, 3);
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"previous"));

  // A rejected edit reports the handler's error text.
  window.SetEditHandler(
      [](const htmlppt::DetailFields&, std::wstring* error) {
        if (error != nullptr) *error = L"html must not be empty";
        return false;
      });
  click_apply();
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status),
                   std::wstring(L"html must not be empty"));

  // No handler installed: the same click is a no-op, not a crash.
  window.SetEditHandler(nullptr);
  click_apply();
  HTMLPPT_CHECK_EQ(calls, 3);
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status),
                   std::wstring(L"html must not be empty"));

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_follows_the_list_selection) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].index = 0; rows[0].slide = 3; rows[0].html = "page.html";
  rows[0].assets_mode = "file"; rows[0].assets_explicit = true;
  rows[0].state = "resolved";
  rows[1].index = 7; rows[1].slide = 5; rows[1].html = "two.html";
  rows[1].state = "resolved";
  window.SetRows(rows);

  // lParam carries the model index, not the position in the list.
  LVITEMW item{};
  item.mask = LVIF_PARAM;
  item.iItem = 1;
  HTMLPPT_CHECK(ListView_GetItem(window.list_view(), &item) != 0);
  HTMLPPT_CHECK_EQ(static_cast<std::size_t>(item.lParam), std::size_t{7});

  // A list selection notification (the same one a mouse click produces) drives
  // SelectRow and fills the pane.
  ListView_SetItemState(window.list_view(), 0, LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
  auto fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"3"));
  HTMLPPT_CHECK_EQ(fields.assets, std::wstring(L"file"));

  ListView_SetItemState(window.list_view(), 1, LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
  fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"5"));

  // An out-of-range row clamps to the last row instead of reading past the
  // model snapshot.
  window.SelectRow(99);
  HTMLPPT_CHECK_EQ(window.detail_fields().slide, std::wstring(L"5"));

  // Clearing the list selection disables the pane; no stale row stays bound.
  ListView_SetItemState(window.list_view(), -1, 0, LVIS_SELECTED);
  HTMLPPT_CHECK(!window.detail_enabled());
  HTMLPPT_CHECK(window.detail_fields().slide.empty());

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_detail_reads_the_model_snapshot) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(1);
  rows[0].index = 0; rows[0].slide = 3; rows[0].label = "Live";
  rows[0].html = "page.html"; rows[0].state = "resolved";
  window.SetRows(rows);

  // The pane must read the model's last snapshot, never the list control text.
  ListView_SetItemText(window.list_view(), 0, 0, const_cast<wchar_t*>(L"999"));
  ListView_SetItemText(window.list_view(), 0, 1,
                       const_cast<wchar_t*>(L"mutated"));
  ListView_SetItemText(window.list_view(), 0, 2,
                       const_cast<wchar_t*>(L"mutated.html"));
  window.SelectRow(0);
  const auto fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"3"));
  HTMLPPT_CHECK_EQ(fields.label, std::wstring(L"Live"));
  HTMLPPT_CHECK_EQ(fields.html, std::wstring(L"page.html"));
  HTMLPPT_CHECK(fields.enabled);

  // A selection that no longer exists must not survive a smaller snapshot.
  window.SetRows(std::vector<htmlppt::DisplayRow>{});
  HTMLPPT_CHECK(!window.detail_enabled());
  HTMLPPT_CHECK(window.detail_fields().slide.empty());

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(config_editor_opens_an_empty_model_for_a_missing_file) {
  const auto result = htmlppt::LoadConfigFile("no-such-watcher.json");
  HTMLPPT_CHECK(!result.config.has_value());
  HTMLPPT_CHECK(!result.issues.empty());
  htmlppt::ConfigEditorModel model{htmlppt::Config{}};
  HTMLPPT_CHECK_EQ(model.target_count(), std::size_t{0});
  HTMLPPT_CHECK_EQ(model.DisplayRows(nullptr, htmlppt::ResolveOptions{}).size(),
                   std::size_t{0});
}

HTMLPPT_TEST_CASE(win32_config_window_keeps_non_ascii_text_intact) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));

  std::vector<htmlppt::DisplayRow> rows(1);
  rows[0].label = "\xE5\xB1\x95\xE7\xA4\xBA";
  rows[0].html = "\xE6\xBC\x94\xE7\xA4\xBA/p.html";
  window.SetRows(rows);

  wchar_t text[128] = {};
  ListView_GetItemText(window.list_view(), 0, 1, text, 128);
  HTMLPPT_CHECK_EQ(std::wstring(text), std::wstring(L"\u5c55\u793a"));
  ListView_GetItemText(window.list_view(), 0, 2, text, 128);
  HTMLPPT_CHECK_EQ(std::wstring(text), std::wstring(L"\u6f14\u793a/p.html"));

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_parse_target_edit_fields_rejects_non_numeric_slide) {
  htmlppt::TargetEdit edit;
  std::wstring error;
  HTMLPPT_CHECK(!htmlppt::ParseTargetEditFields(L"x", L"page1.html", L"", &edit,
                                               &error));
  HTMLPPT_CHECK(!error.empty());
  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(L"7", L"page1.html", L"Live",
                                               &edit, &error));
  HTMLPPT_CHECK_EQ(edit.slide, 7);
  HTMLPPT_CHECK_EQ(edit.label, std::string{"Live"});
  // CJK path survives wide -> UTF-8.
  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(
      L"1", L"\u6f14\u793a/p.html", L"", &edit, &error));
  HTMLPPT_CHECK_EQ(edit.html, std::string{"\xE6\xBC\x94\xE7\xA4\xBA/p.html"});
}

HTMLPPT_TEST_CASE(win32_parse_target_edit_fields_rejects_a_partial_number) {
  htmlppt::TargetEdit edit;
  std::wstring error;
  // An empty slide field is a parse error, not a silent default.
  HTMLPPT_CHECK(!htmlppt::ParseTargetEditFields(L"", L"page1.html", L"", &edit,
                                               &error));
  HTMLPPT_CHECK(!error.empty());
  // from_chars must consume the whole field, not just its leading digits.
  HTMLPPT_CHECK(!htmlppt::ParseTargetEditFields(L"7x", L"page1.html", L"", &edit,
                                               &error));
  HTMLPPT_CHECK(!htmlppt::ParseTargetEditFields(L" 7", L"page1.html", L"", &edit,
                                               &error));
  // A successful parse clears the stale error text.
  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(L"3", L"page1.html", L"", &edit,
                                               &error));
  HTMLPPT_CHECK(error.empty());
  HTMLPPT_CHECK_EQ(edit.slide, 3);
}

HTMLPPT_TEST_CASE(win32_target_edit_from_detail_fields_maps_the_switch_and_mode) {
  htmlppt::DetailFields fields;
  fields.slide = L"3";
  fields.html = L"page.html";
  fields.label = L"Live";
  fields.assets = L"tree";
  fields.enabled = false;
  htmlppt::TargetEdit edit;
  std::wstring error;
  HTMLPPT_CHECK(htmlppt::TargetEditFromDetailFields(fields, &edit, &error));
  HTMLPPT_CHECK_EQ(edit.slide, 3);
  HTMLPPT_CHECK_EQ(edit.html, std::string{"page.html"});
  HTMLPPT_CHECK_EQ(edit.label, std::string{"Live"});
  HTMLPPT_CHECK_EQ(edit.assets_mode, std::string{"tree"});
  HTMLPPT_CHECK(!edit.enabled);

  // The pane's "inherit" is an explicit choice, so it is forwarded as the
  // model's clear sentinel rather than as an empty "leave untouched" mode.
  fields.assets = L"inherit";
  HTMLPPT_CHECK(htmlppt::TargetEditFromDetailFields(fields, &edit, &error));
  HTMLPPT_CHECK_EQ(edit.assets_mode, std::string{"inherit"});

  // An empty combo text still means "leave the target's block untouched".
  fields.assets.clear();
  HTMLPPT_CHECK(htmlppt::TargetEditFromDetailFields(fields, &edit, &error));
  HTMLPPT_CHECK(edit.assets_mode.empty());

  // A bad slide surfaces the parser's message instead of a half-filled edit.
  fields.slide = L"x";
  HTMLPPT_CHECK(!htmlppt::TargetEditFromDetailFields(fields, &edit, &error));
  HTMLPPT_CHECK(!error.empty());
}

HTMLPPT_TEST_CASE(win32_config_window_reports_the_selected_model_index) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].index = 0; rows[0].slide = 3; rows[0].html = "page.html";
  rows[0].state = "resolved";
  rows[1].index = 7; rows[1].slide = 5; rows[1].html = "two.html";
  rows[1].state = "resolved";
  window.SetRows(rows);

  std::vector<int> reported;
  window.SetSelectionHandler(
      [&reported](int index) { reported.push_back(index); });
  // A list selection notification (the same one a click produces) reports the
  // row's model index, not its position in the list.
  ListView_SetItemState(window.list_view(), 1, LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
  HTMLPPT_CHECK(!reported.empty());
  HTMLPPT_CHECK_EQ(reported.back(), 7);

  // Clearing the selection reports that there is nothing to apply.
  window.SelectRow(-1);
  HTMLPPT_CHECK_EQ(reported.back(), -1);

  // A refresh that drops the selected row must report the cleared selection,
  // so the app cannot keep applying edits to a vanished target.
  ListView_SetItemState(window.list_view(), 0, LVIS_SELECTED, LVIS_SELECTED);
  HTMLPPT_CHECK_EQ(reported.back(), 0);
  window.SetRows(std::vector<htmlppt::DisplayRow>{});
  HTMLPPT_CHECK_EQ(reported.back(), -1);

  // Removing the handler stops the callbacks instead of crashing on a click.
  std::size_t calls = reported.size();
  window.SetSelectionHandler(nullptr);
  window.SetRows(rows);
  window.SelectRow(0);
  HTMLPPT_CHECK_EQ(reported.size(), calls);

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_routes_the_undo_and_redo_keys) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));

  int undos = 0;
  int redos = 0;
  window.SetUndoHandler([&undos](std::wstring* error) {
    ++undos;
    if (error != nullptr) error->clear();
    return true;
  });
  window.SetRedoHandler([&redos](std::wstring* error) {
    ++redos;
    if (error != nullptr) error->clear();
    return true;
  });

  // The pure routing the message routes call: outside an EDIT, Ctrl+Z undoes,
  // Ctrl+Y and Ctrl+Shift+Z redo, and every other key falls through.
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, false, 'Z'));
  HTMLPPT_CHECK_EQ(undos, 1);
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, false, 'Y'));
  HTMLPPT_CHECK_EQ(redos, 1);
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, true, 'Z'));
  HTMLPPT_CHECK_EQ(redos, 2);
  HTMLPPT_CHECK_EQ(undos, 1);

  // Focus in a detail EDIT: plain Ctrl+Z belongs to the control's own text undo
  // and is not consumed, so the user does not lose in-field undo. Ctrl+Y and
  // Ctrl+Shift+Z stay editor redo because the single-line EDIT has no native
  // redo.
  HTMLPPT_CHECK(!window.HandleEditorKey(true, true, false, 'Z'));
  HTMLPPT_CHECK_EQ(undos, 1);
  HTMLPPT_CHECK(window.HandleEditorKey(true, true, false, 'Y'));
  HTMLPPT_CHECK_EQ(redos, 3);
  HTMLPPT_CHECK(window.HandleEditorKey(true, true, true, 'Z'));
  HTMLPPT_CHECK_EQ(redos, 4);
  HTMLPPT_CHECK_EQ(undos, 1);

  HTMLPPT_CHECK(!window.HandleEditorKey(false, false, false, 'Z'));
  HTMLPPT_CHECK(!window.HandleEditorKey(false, false, true, 'Z'));
  HTMLPPT_CHECK(!window.HandleEditorKey(false, true, false, 'A'));
  HTMLPPT_CHECK(!window.HandleEditorKey(true, false, false, 'Z'));
  HTMLPPT_CHECK(!window.HandleEditorKey(true, false, false, 'Y'));
  HTMLPPT_CHECK_EQ(undos, 1);
  HTMLPPT_CHECK_EQ(redos, 4);

  // The WM_KEYDOWN route runs the same routing; without a physical Ctrl key
  // (a test cannot fake the thread's key state) an unrelated key must not
  // touch the history.
  SendMessageW(window.handle(), WM_KEYDOWN, 'A', 0);
  HTMLPPT_CHECK_EQ(undos, 1);

  // A refused undo shows the app's failure reason in the status line.
  window.SetUndoHandler([](std::wstring* error) {
    if (error != nullptr) *error = L"nothing to undo";
    return false;
  });
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, false, 'Z'));
  wchar_t status[64] = {};
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"nothing to undo"));

  // A failure without a message still gets a view-owned fallback.
  window.SetUndoHandler([](std::wstring*) { return false; });
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, false, 'Z'));
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"cannot undo"));

  // A focused EDIT does not consume Ctrl+Z, so it never reaches (or reports
  // from) the undo handler at all.
  window.SetStatus(L"unchanged");
  HTMLPPT_CHECK(!window.HandleEditorKey(true, true, false, 'Z'));
  GetWindowTextW(window.status_view(), status, 64);
  HTMLPPT_CHECK_EQ(std::wstring(status), std::wstring(L"unchanged"));

  // With no handler installed the shortcut is a no-op rather than a crash.
  window.SetUndoHandler(nullptr);
  window.SetRedoHandler(nullptr);
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, false, 'Z'));
  HTMLPPT_CHECK(window.HandleEditorKey(false, true, false, 'Y'));
  HTMLPPT_CHECK_EQ(undos, 1);
  HTMLPPT_CHECK_EQ(redos, 4);

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
}

HTMLPPT_TEST_CASE(win32_should_close_window_honours_dirty_and_answer) {
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(false, false));
  HTMLPPT_CHECK(!htmlppt::ShouldCloseWindow(true, false));
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(true, true));
}

HTMLPPT_TEST_CASE(win32_classify_drop_decides_by_kind_and_selection) {
  // A file dropped on a selected row fills the HTML field and applies the edit.
  HTMLPPT_CHECK(htmlppt::ClassifyDrop(false, true) ==
                htmlppt::DropDecision::ApplyHtml);
  // A file with no selection still fills the field, but the view asks for a
  // target instead of applying an edit to nothing.
  HTMLPPT_CHECK(htmlppt::ClassifyDrop(false, false) ==
                htmlppt::DropDecision::RejectNoSelection);
  // A folder is never an .html file, with or without a selection; the directory
  // rejection wins so the status names the real problem.
  HTMLPPT_CHECK(htmlppt::ClassifyDrop(true, true) ==
                htmlppt::DropDecision::RejectDirectory);
  HTMLPPT_CHECK(htmlppt::ClassifyDrop(true, false) ==
                htmlppt::DropDecision::RejectDirectory);
}

HTMLPPT_TEST_CASE(win32_config_window_drops_an_html_file_onto_the_selected_row) {
  const std::wstring html_path = UniqueTempPath(L".html");
  HTMLPPT_CHECK(!html_path.empty());
  const HANDLE file =
      CreateFileW(html_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  HTMLPPT_CHECK(file != INVALID_HANDLE_VALUE);
  CloseHandle(file);

  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  // DragAcceptFiles marks the window with WS_EX_ACCEPTFILES, which is how the
  // shell knows to deliver a drop to it.
  HTMLPPT_CHECK((GetWindowLongPtrW(window.handle(), GWL_EXSTYLE) &
                 WS_EX_ACCEPTFILES) != 0);

  std::vector<htmlppt::DisplayRow> rows(1);
  rows[0].index = 0; rows[0].slide = 3; rows[0].label = "Live";
  rows[0].html = "page.html"; rows[0].state = "resolved";
  window.SetRows(rows);
  window.SelectRow(0);

  htmlppt::DetailFields captured;
  int calls = 0;
  window.SetEditHandler(
      [&](const htmlppt::DetailFields& fields, std::wstring* error) {
        captured = fields;
        ++calls;
        if (error != nullptr) error->clear();
        window.SetStatus(L"applied");
        return true;
      });
  SendDrop(window.handle(), html_path);

  // The dropped path fills the HTML field and the installed handler applies it,
  // with the rest of the pane unchanged.
  HTMLPPT_CHECK_EQ(calls, 1);
  HTMLPPT_CHECK_EQ(window.detail_fields().html, html_path);
  HTMLPPT_CHECK_EQ(captured.html, html_path);
  HTMLPPT_CHECK_EQ(captured.slide, std::wstring(L"3"));
  HTMLPPT_CHECK_EQ(captured.label, std::wstring(L"Live"));
  // The apply path owns the status; the view adds nothing of its own.
  HTMLPPT_CHECK_EQ(StatusText(window.status_view()), std::wstring(L"applied"));

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
  DeleteFileW(html_path.c_str());
}

HTMLPPT_TEST_CASE(win32_config_window_drop_without_a_selection_asks_for_a_target) {
  const std::wstring html_path = UniqueTempPath(L".html");
  HTMLPPT_CHECK(!html_path.empty());
  const HANDLE file =
      CreateFileW(html_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  HTMLPPT_CHECK(file != INVALID_HANDLE_VALUE);
  CloseHandle(file);

  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(1);
  rows[0].index = 0; rows[0].slide = 3; rows[0].html = "page.html";
  rows[0].state = "resolved";
  window.SetRows(rows);
  // SetRows keeps no selection: the pane is disabled and must not be applied.
  HTMLPPT_CHECK(!window.detail_enabled());

  int calls = 0;
  window.SetEditHandler([&calls](const htmlppt::DetailFields&,
                                 std::wstring* error) {
    ++calls;
    if (error != nullptr) error->clear();
    return true;
  });
  SendDrop(window.handle(), html_path);

  // The field still shows the file (so the user can pick a row and continue),
  // but no edit is applied and the status asks for a target.
  HTMLPPT_CHECK_EQ(calls, 0);
  HTMLPPT_CHECK_EQ(window.detail_fields().html, html_path);
  HTMLPPT_CHECK_EQ(StatusText(window.status_view()),
                   std::wstring(L"select a target first"));

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
  DeleteFileW(html_path.c_str());
}

HTMLPPT_TEST_CASE(win32_config_window_rejects_a_dropped_folder) {
  const std::wstring directory = UniqueTempPath(L"-folder");
  HTMLPPT_CHECK(!directory.empty());
  HTMLPPT_CHECK(CreateDirectoryW(directory.c_str(), nullptr) != 0);

  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(1);
  rows[0].index = 0; rows[0].slide = 3; rows[0].html = "page.html";
  rows[0].state = "resolved";
  window.SetRows(rows);
  window.SelectRow(0);

  int calls = 0;
  window.SetEditHandler([&calls](const htmlppt::DetailFields&,
                                 std::wstring* error) {
    ++calls;
    if (error != nullptr) error->clear();
    return true;
  });
  SendDrop(window.handle(), directory);

  // A folder is refused before it touches the fields or the model.
  HTMLPPT_CHECK_EQ(calls, 0);
  HTMLPPT_CHECK_EQ(window.detail_fields().html, std::wstring(L"page.html"));
  HTMLPPT_CHECK_EQ(StatusText(window.status_view()),
                   std::wstring(L"drop an .html file, not a folder"));

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
  RemoveDirectoryW(directory.c_str());
}

HTMLPPT_TEST_CASE(win32_config_window_close_policy_prompts_only_when_dirty) {
  htmlppt::ConfigWindowOptions options;
  options.show = false;

  // A clean window closes without asking and without saving.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    HTMLPPT_CHECK(!window.dirty());
    int prompts = 0;
    int saves = 0;
    window.SetClosePromptHandler([&prompts] {
      ++prompts;
      return IDCANCEL;
    });
    window.SetSaveHandler([&saves](std::wstring* error) {
      ++saves;
      if (error != nullptr) error->clear();
      return true;
    });
    HTMLPPT_CHECK(window.Close());
    PumpUntilDestroyed(window.handle());
    HTMLPPT_CHECK(window.handle() == nullptr);
    HTMLPPT_CHECK_EQ(prompts, 0);
    HTMLPPT_CHECK_EQ(saves, 0);
  }

  // Cancel keeps the window and its edits, and never runs the save handler.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    window.SetDirty(true);
    HTMLPPT_CHECK(window.dirty());
    window.SetDirty(false);
    HTMLPPT_CHECK(!window.dirty());
    window.SetDirty(true);
    int prompts = 0;
    int saves = 0;
    window.SetClosePromptHandler([&prompts] {
      ++prompts;
      return IDCANCEL;
    });
    window.SetSaveHandler([&saves](std::wstring* error) {
      ++saves;
      if (error != nullptr) error->clear();
      return true;
    });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() != nullptr);
    HTMLPPT_CHECK_EQ(prompts, 1);
    HTMLPPT_CHECK_EQ(saves, 0);
    // Leave the window clean so the destructor (not WM_CLOSE) tears it down.
    window.SetDirty(false);
  }

  // No discards the edits and closes without saving.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    window.SetDirty(true);
    int saves = 0;
    window.SetClosePromptHandler([] { return IDNO; });
    window.SetSaveHandler([&saves](std::wstring* error) {
      ++saves;
      if (error != nullptr) error->clear();
      return true;
    });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() == nullptr);
    HTMLPPT_CHECK_EQ(saves, 0);
  }

  // Yes saves and closes once the save succeeded. A dirty, error-free model is
  // exactly the state with the Save gate open.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    window.SetDirty(true);
    window.SetSaveEnabled(true);
    int saves = 0;
    window.SetClosePromptHandler([] { return IDYES; });
    window.SetSaveHandler([&saves](std::wstring* error) {
      ++saves;
      if (error != nullptr) error->clear();
      return true;
    });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() == nullptr);
    HTMLPPT_CHECK_EQ(saves, 1);
  }

  // The close prompt respects the Save gate: a dirty config with a blocking
  // error is not persisted, so Yes keeps the window and never saves.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    window.SetDirty(true);
    window.SetSaveEnabled(false);
    window.SetStatus(L"slide 5 is already mapped by target 1");
    int saves = 0;
    window.SetClosePromptHandler([] { return IDYES; });
    window.SetSaveHandler([&saves](std::wstring* error) {
      ++saves;
      if (error != nullptr) error->clear();
      return true;
    });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() != nullptr);
    HTMLPPT_CHECK_EQ(saves, 0);
    wchar_t status[64] = {};
    GetWindowTextW(window.status_view(), status, 64);
    HTMLPPT_CHECK_EQ(std::wstring(status),
                     std::wstring(L"slide 5 is already mapped by target 1"));
    window.SetDirty(false);
  }

  // A failed save keeps the dirty window open and shows why.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    window.SetDirty(true);
    window.SetSaveEnabled(true);
    int saves = 0;
    window.SetClosePromptHandler([] { return IDYES; });
    window.SetSaveHandler([&saves](std::wstring* error) {
      ++saves;
      if (error != nullptr) *error = L"cannot write watcher.json";
      return false;
    });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() != nullptr);
    HTMLPPT_CHECK_EQ(saves, 1);
    wchar_t status[64] = {};
    GetWindowTextW(window.status_view(), status, 64);
    HTMLPPT_CHECK_EQ(std::wstring(status),
                     std::wstring(L"cannot write watcher.json"));
    // Answer No to discard on teardown.
    window.SetClosePromptHandler([] { return IDNO; });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() == nullptr);
  }

  // "Yes" without a save handler cannot honour the answer and keeps the window.
  {
    htmlppt::ConfigWindow window;
    HTMLPPT_CHECK(window.Create(options));
    window.SetDirty(true);
    window.SetSaveEnabled(true);
    int prompts = 0;
    window.SetClosePromptHandler([&prompts] {
      ++prompts;
      return IDYES;
    });
    SendMessageW(window.handle(), WM_CLOSE, 0, 0);
    HTMLPPT_CHECK(window.handle() != nullptr);
    HTMLPPT_CHECK_EQ(prompts, 1);
    wchar_t status[64] = {};
    GetWindowTextW(window.status_view(), status, 64);
    HTMLPPT_CHECK_EQ(std::wstring(status),
                     std::wstring(L"cannot save before closing"));
    window.SetDirty(false);
  }
}

HTMLPPT_TEST_CASE(win32_edit_status_text_reports_the_blocking_reason) {
  // A clean refresh means the edit was applied; a blocked refresh reports the
  // first reason verbatim instead of a blanket "applied".
  HTMLPPT_CHECK_EQ(htmlppt::EditStatusText(L""), std::wstring(L"applied"));
  HTMLPPT_CHECK_EQ(htmlppt::EditStatusText(L"deck must not be empty"),
                   std::wstring(L"deck must not be empty"));
}

HTMLPPT_TEST_CASE(win32_config_window_relayouts_on_resize) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));

  SendMessageW(window.handle(), WM_SIZE, SIZE_RESTORED, MAKELPARAM(600, 400));

  const UINT dpi = window.dpi();
  HTMLPPT_CHECK(dpi >= 96);

  RECT list_rect{};
  HTMLPPT_CHECK(GetWindowRect(window.list_view(), &list_rect) != 0);
  MapWindowPoints(nullptr, window.handle(),
                  reinterpret_cast<POINT*>(&list_rect), 2);
  // The toolbar owns a 36 px band across the top, so the list starts below it.
  HTMLPPT_CHECK_EQ(list_rect.left, 0);
  HTMLPPT_CHECK_EQ(list_rect.top, Scaled(36, dpi));
  // The detail pane owns a fixed 300 px right-hand column.
  HTMLPPT_CHECK_EQ(list_rect.right - list_rect.left, 600 - Scaled(300, dpi));
  HTMLPPT_CHECK_EQ(list_rect.bottom - list_rect.top,
                   400 - Scaled(24, dpi) - Scaled(36, dpi));

  RECT status_rect{};
  HTMLPPT_CHECK(GetWindowRect(window.status_view(), &status_rect) != 0);
  MapWindowPoints(nullptr, window.handle(),
                  reinterpret_cast<POINT*>(&status_rect), 2);
  HTMLPPT_CHECK_EQ(status_rect.left, 0);
  HTMLPPT_CHECK_EQ(status_rect.top, 400 - Scaled(24, dpi));
  HTMLPPT_CHECK_EQ(status_rect.right - status_rect.left, 600);
  HTMLPPT_CHECK_EQ(status_rect.bottom - status_rect.top, Scaled(24, dpi));

  // Even below the window's minimum (a programmatic resize bypasses the
  // tracking size), the Apply button is bottom-anchored above the status line.
  RECT apply_rect{};
  HTMLPPT_CHECK(GetWindowRect(window.apply_button(), &apply_rect) != 0);
  MapWindowPoints(nullptr, window.handle(),
                  reinterpret_cast<POINT*>(&apply_rect), 2);
  HTMLPPT_CHECK(apply_rect.bottom <= status_rect.top);
  HTMLPPT_CHECK(apply_rect.left >= 600 - Scaled(300, dpi));
  HTMLPPT_CHECK(apply_rect.right <= 600);

  // The window declares a minimum tracking size that fits the toolbar, the
  // whole detail pane, and the status line.
  MINMAXINFO minimum{};
  SendMessageW(window.handle(), WM_GETMINMAXINFO, 0,
               reinterpret_cast<LPARAM>(&minimum));
  HTMLPPT_CHECK(minimum.ptMinTrackSize.x >= Scaled(560, dpi));
  HTMLPPT_CHECK(minimum.ptMinTrackSize.y >= Scaled(424, dpi));

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_scales_with_the_dpi) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.width = 900;
  options.height = 560;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));

  const UINT initial = window.dpi();
  HTMLPPT_CHECK(initial >= 96);
  // The initial size is expressed in 96-DPI units and scaled to the window's
  // own DPI.
  RECT created{};
  HTMLPPT_CHECK(GetWindowRect(window.handle(), &created) != 0);
  HTMLPPT_CHECK_EQ(created.right - created.left, Scaled(900, initial));
  HTMLPPT_CHECK_EQ(created.bottom - created.top, Scaled(560, initial));

  // Fonts must scale with the DPI as well: geometry-only scaling left the text
  // at its old size and clipped it inside the controls.
  RECT at_96{40, 40, 40 + 1000, 40 + 700};
  SendMessageW(window.handle(), WM_DPICHANGED, MAKEWPARAM(96, 96),
               reinterpret_cast<LPARAM>(&at_96));
  HTMLPPT_CHECK_EQ(window.dpi(), UINT{96});
  const int font_96_list = FontHeight(window.list_view());
  const int font_96_button = FontHeight(window.save_button());
  HTMLPPT_CHECK(font_96_list != 0);
  HTMLPPT_CHECK(font_96_button != 0);

  // A DPI change moves the window onto the suggested rect and re-lays the
  // controls out at the new DPI.
  RECT suggested{40, 40, 40 + 1000, 40 + 700};
  SendMessageW(window.handle(), WM_DPICHANGED, MAKEWPARAM(144, 144),
               reinterpret_cast<LPARAM>(&suggested));
  HTMLPPT_CHECK_EQ(window.dpi(), UINT{144});
  RECT moved{};
  HTMLPPT_CHECK(GetWindowRect(window.handle(), &moved) != 0);
  HTMLPPT_CHECK_EQ(moved.left, 40);
  HTMLPPT_CHECK_EQ(moved.top, 40);
  HTMLPPT_CHECK_EQ(moved.right - moved.left, 1000);
  HTMLPPT_CHECK_EQ(moved.bottom - moved.top, 700);

  // The font is recreated and re-applied to every child on the DPI change; the
  // ListView and a toolbar button both carry the new, taller font.
  const int font_144_list = FontHeight(window.list_view());
  const int font_144_button = FontHeight(window.save_button());
  HTMLPPT_CHECK(font_144_list != 0);
  HTMLPPT_CHECK(font_144_button != 0);
  HTMLPPT_CHECK(font_96_list != font_144_list);
  HTMLPPT_CHECK(font_96_button != font_144_button);

  RECT client{};
  HTMLPPT_CHECK(GetClientRect(window.handle(), &client) != 0);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  HTMLPPT_CHECK(width > 0);
  HTMLPPT_CHECK(height > 0);
  const UINT dpi = window.dpi();

  RECT list_rect{};
  HTMLPPT_CHECK(GetWindowRect(window.list_view(), &list_rect) != 0);
  MapWindowPoints(nullptr, window.handle(),
                  reinterpret_cast<POINT*>(&list_rect), 2);
  HTMLPPT_CHECK_EQ(list_rect.left, 0);
  HTMLPPT_CHECK_EQ(list_rect.top, Scaled(36, dpi));
  HTMLPPT_CHECK_EQ(list_rect.right - list_rect.left, width - Scaled(300, dpi));
  HTMLPPT_CHECK_EQ(list_rect.bottom - list_rect.top,
                   height - Scaled(24, dpi) - Scaled(36, dpi));

  RECT status_rect{};
  HTMLPPT_CHECK(GetWindowRect(window.status_view(), &status_rect) != 0);
  MapWindowPoints(nullptr, window.handle(),
                  reinterpret_cast<POINT*>(&status_rect), 2);
  HTMLPPT_CHECK_EQ(status_rect.left, 0);
  HTMLPPT_CHECK_EQ(status_rect.top, height - Scaled(24, dpi));
  HTMLPPT_CHECK_EQ(status_rect.right - status_rect.left, width);
  HTMLPPT_CHECK_EQ(status_rect.bottom - status_rect.top, Scaled(24, dpi));

  RECT apply_rect{};
  HTMLPPT_CHECK(GetWindowRect(window.apply_button(), &apply_rect) != 0);
  MapWindowPoints(nullptr, window.handle(),
                  reinterpret_cast<POINT*>(&apply_rect), 2);
  HTMLPPT_CHECK(apply_rect.bottom <= status_rect.top);
  HTMLPPT_CHECK(apply_rect.left >= width - Scaled(300, dpi));
  HTMLPPT_CHECK(apply_rect.right <= width);

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}

HTMLPPT_TEST_CASE(win32_config_window_dark_theme_uses_dark_surfaces) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  options.theme = htmlppt::ConfigWindowTheme::Dark;
  HTMLPPT_CHECK(window.Create(options));
  HTMLPPT_CHECK(window.list_view() != nullptr);

  // The list body, the item label background, and the item text all come from
  // the one dark colour pair.
  HTMLPPT_CHECK_EQ(ListView_GetBkColor(window.list_view()),
                   htmlppt::kConfigWindowDarkColors.background);
  HTMLPPT_CHECK_EQ(ListView_GetTextBkColor(window.list_view()),
                   htmlppt::kConfigWindowDarkColors.background);
  HTMLPPT_CHECK_EQ(ListView_GetTextColor(window.list_view()),
                   htmlppt::kConfigWindowDarkColors.text);

  // The status line, a detail EDIT, and the assets drop-down list paint on the
  // dark surface with light text (the brush and the DC's text colour).
  COLORREF status_text = CLR_INVALID;
  const std::optional<COLORREF> status_background = ControlSurfaceColor(
      window.handle(), WM_CTLCOLORSTATIC, window.status_view(), &status_text);
  HTMLPPT_CHECK(status_background.has_value());
  HTMLPPT_CHECK_EQ(*status_background,
                   htmlppt::kConfigWindowDarkColors.background);
  HTMLPPT_CHECK_EQ(status_text, htmlppt::kConfigWindowDarkColors.text);

  HWND edit = FindWindowExW(window.handle(), nullptr, L"EDIT", nullptr);
  HTMLPPT_CHECK(edit != nullptr);
  const std::optional<COLORREF> edit_background =
      ControlSurfaceColor(window.handle(), WM_CTLCOLOREDIT, edit);
  HTMLPPT_CHECK(edit_background.has_value());
  HTMLPPT_CHECK_EQ(*edit_background,
                   htmlppt::kConfigWindowDarkColors.background);

  HWND combo = FindWindowExW(window.handle(), nullptr, L"COMBOBOX", nullptr);
  HTMLPPT_CHECK(combo != nullptr);
  const std::optional<COLORREF> combo_background =
      ControlSurfaceColor(window.handle(), WM_CTLCOLORLISTBOX, combo);
  HTMLPPT_CHECK(combo_background.has_value());
  HTMLPPT_CHECK_EQ(*combo_background,
                   htmlppt::kConfigWindowDarkColors.background);

  // The window itself erases to the dark surface, so a resize never flashes a
  // light rectangle around the themed children.
  const std::optional<COLORREF> erased =
      ErasedBackgroundColor(window.handle());
  HTMLPPT_CHECK(erased.has_value());
  HTMLPPT_CHECK_EQ(*erased, htmlppt::kConfigWindowDarkColors.background);

  // The immersive dark title bar is a DWM attribute; a Windows build without it
  // cannot honour the request, so the assertion is skipped there.
  const std::optional<bool> title_bar_dark =
      QueryDarkTitleBar(window.handle());
  if (title_bar_dark.has_value()) {
    HTMLPPT_CHECK(*title_bar_dark);
    HTMLPPT_CHECK(htmlppt::IsDarkTitleBarEnabled(window.handle()));
  }

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
}

HTMLPPT_TEST_CASE(win32_config_window_light_theme_keeps_light_surfaces) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  options.theme = htmlppt::ConfigWindowTheme::Light;
  HTMLPPT_CHECK(window.Create(options));

  // White, the CLR_WINDOW default, is untouched.
  HTMLPPT_CHECK_EQ(ListView_GetBkColor(window.list_view()), RGB(255, 255, 255));
  HTMLPPT_CHECK_EQ(ListView_GetBkColor(window.list_view()),
                   htmlppt::kConfigWindowLightColors.background);
  HTMLPPT_CHECK_EQ(ListView_GetTextBkColor(window.list_view()),
                   htmlppt::kConfigWindowLightColors.background);
  HTMLPPT_CHECK_EQ(ListView_GetTextColor(window.list_view()),
                   htmlppt::kConfigWindowLightColors.text);

  COLORREF status_text = CLR_INVALID;
  const std::optional<COLORREF> status_background = ControlSurfaceColor(
      window.handle(), WM_CTLCOLORSTATIC, window.status_view(), &status_text);
  HTMLPPT_CHECK(status_background.has_value());
  HTMLPPT_CHECK_EQ(*status_background,
                   htmlppt::kConfigWindowLightColors.background);
  HTMLPPT_CHECK_EQ(status_text, htmlppt::kConfigWindowLightColors.text);

  const std::optional<COLORREF> erased =
      ErasedBackgroundColor(window.handle());
  HTMLPPT_CHECK(erased.has_value());
  HTMLPPT_CHECK_EQ(*erased, htmlppt::kConfigWindowLightColors.background);

  // An explicit light theme turns the immersive dark title bar back off.
  const std::optional<bool> title_bar_dark =
      QueryDarkTitleBar(window.handle());
  if (title_bar_dark.has_value()) {
    HTMLPPT_CHECK(!*title_bar_dark);
    HTMLPPT_CHECK(!htmlppt::IsDarkTitleBarEnabled(window.handle()));
  }

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
}

HTMLPPT_TEST_CASE(win32_config_window_system_theme_resolves_the_registry_value) {
  // AppsUseLightTheme: 0 means dark; 1 (or any non-zero) and a missing value
  // mean light.
  HTMLPPT_CHECK(htmlppt::DarkFromAppsUseLightTheme(0));
  HTMLPPT_CHECK(!htmlppt::DarkFromAppsUseLightTheme(1));
  HTMLPPT_CHECK(!htmlppt::DarkFromAppsUseLightTheme(std::nullopt));

  // The default option, System, reads the current user's app mode. Compare the
  // window against that same registry value instead of assuming a machine.
  const std::optional<DWORD> apps_use_light = ReadAppsUseLightThemeForTest();
  const bool expect_dark = htmlppt::DarkFromAppsUseLightTheme(apps_use_light);
  const htmlppt::ConfigWindowColors& expected =
      expect_dark ? htmlppt::kConfigWindowDarkColors
                  : htmlppt::kConfigWindowLightColors;
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  HTMLPPT_CHECK_EQ(ListView_GetBkColor(window.list_view()),
                   expected.background);

  window.Close();
  PumpUntilDestroyed(window.handle());
  HTMLPPT_CHECK(window.handle() == nullptr);
}
