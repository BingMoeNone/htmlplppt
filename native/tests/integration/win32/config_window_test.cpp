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
  HTMLPPT_CHECK(std::wstring(text).rfind(L"error", 0) == 0);

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