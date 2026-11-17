#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

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

HTMLPPT_TEST_CASE(win32_should_close_window_honours_dirty_and_answer) {
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(false, false));
  HTMLPPT_CHECK(!htmlppt::ShouldCloseWindow(true, false));
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(true, true));
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