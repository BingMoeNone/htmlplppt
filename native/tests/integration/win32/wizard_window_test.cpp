#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <commctrl.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/mapping_wizard.h"
#include "core/utf8_path.h"
#include "platform/win32/utf8_win32.h"
#include "platform/win32/wizard_window.h"
#include "test_utils.h"

using htmlppt::Utf8Path;
using htmlppt::WizardPair;
using htmlppt::WizardSlide;
using htmlppt::WizardWindow;
using htmlppt::WizardWindowOptions;

namespace {

void MakeHtmlFile(const std::filesystem::path& path) {
  std::ofstream(path) << "<html></html>";
}

std::string ItemTextUtf8(HWND list, int row, int column) {
  wchar_t buffer[1024] = {};
  ListView_GetItemText(list, row, column, buffer, 1024);
  return htmlppt::WideToUtf8(buffer).value_or("");
}

std::wstring ColumnText(HWND list, int column) {
  wchar_t buffer[64] = {};
  LVCOLUMNW info{};
  info.mask = LVCF_TEXT;
  info.pszText = buffer;
  info.cchTextMax = 64;
  ListView_GetColumn(list, column, &info);
  return buffer;
}

std::wstring StatusText(HWND status_view) {
  wchar_t status[256] = {};
  GetWindowTextW(status_view, status, 256);
  return status;
}

// The same WM_COMMAND/BN_CLICKED route a real click uses.
void Click(HWND parent, HWND button) {
  SendMessageW(parent, WM_COMMAND,
               MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED),
               reinterpret_cast<LPARAM>(button));
}

} // namespace

HTMLPPT_TEST_CASE(win32_wizard_window_lists_the_proposed_pairs) {
  const htmlppt::test::TempDir dir;
  MakeHtmlFile(dir.path() / "slide-1.html");
  MakeHtmlFile(dir.path() / "page-02.HTM");
  MakeHtmlFile(dir.path() / "notes.txt");

  WizardWindowOptions options;
  options.show = false;  // never flash a window in a test run
  options.title = L"htmlppt mapping wizard";
  options.slides = {WizardSlide{1, "One"}, WizardSlide{2, "Two"},
                    WizardSlide{3, "Three"}};
  options.html_folder = dir.path();

  WizardWindow wizard;
  HTMLPPT_CHECK(wizard.Create(options));
  HTMLPPT_CHECK(wizard.handle() != nullptr);
  HTMLPPT_CHECK(wizard.list_view() != nullptr);
  HTMLPPT_CHECK(wizard.status_view() != nullptr);
  HTMLPPT_CHECK(wizard.import_button() != nullptr);
  HTMLPPT_CHECK(wizard.cancel_button() != nullptr);

  HTMLPPT_CHECK_EQ(std::wstring(ColumnText(wizard.list_view(), 0)),
                   std::wstring(L"SLIDE"));
  HTMLPPT_CHECK_EQ(std::wstring(ColumnText(wizard.list_view(), 1)),
                   std::wstring(L"TITLE"));
  HTMLPPT_CHECK_EQ(std::wstring(ColumnText(wizard.list_view(), 2)),
                   std::wstring(L"HTML"));
  HTMLPPT_CHECK_EQ(ListView_GetItemCount(wizard.list_view()), 2);

  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 0, 0), std::string{"1"});
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 0, 1), std::string{"One"});
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 0, 2),
                   Utf8Path(dir.path() / "slide-1.html"));
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 1, 0), std::string{"2"});
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 1, 1), std::string{"Two"});
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 1, 2),
                   Utf8Path(dir.path() / "page-02.HTM"));

  // Every proposal starts checked; the preview is a suggestion until Import.
  HTMLPPT_CHECK(ListView_GetCheckState(wizard.list_view(), 0) != 0);
  HTMLPPT_CHECK(ListView_GetCheckState(wizard.list_view(), 1) != 0);
  HTMLPPT_CHECK_EQ(wizard.pairs().size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(wizard.checked_pairs().size(), std::size_t{2});
  HTMLPPT_CHECK(!StatusText(wizard.status_view()).empty());
}

HTMLPPT_TEST_CASE(win32_wizard_window_imports_only_the_checked_pairs) {
  const htmlppt::test::TempDir dir;
  MakeHtmlFile(dir.path() / "slide-1.html");
  MakeHtmlFile(dir.path() / "slide-2.html");
  MakeHtmlFile(dir.path() / "slide-3.html");

  WizardWindowOptions options;
  options.show = false;
  options.slides = {WizardSlide{1, "One"}, WizardSlide{2, "Two"},
                    WizardSlide{3, "Three"}};
  options.html_folder = dir.path();

  WizardWindow wizard;
  HTMLPPT_CHECK(wizard.Create(options));
  HTMLPPT_CHECK_EQ(ListView_GetItemCount(wizard.list_view()), 3);

  // The user unchecks the first two rows: the manual skip is part of the
  // preview, and only the remaining row is written.
  ListView_SetCheckState(wizard.list_view(), 0, FALSE);
  ListView_SetCheckState(wizard.list_view(), 1, FALSE);
  const auto checked = wizard.checked_pairs();
  HTMLPPT_CHECK_EQ(checked.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(checked[0].slide, 3);

  const HWND import_button = wizard.import_button();
  Click(wizard.handle(), import_button);
  // Import closes the wizard and records the accepted result for the app.
  HTMLPPT_CHECK(wizard.handle() == nullptr);
  HTMLPPT_CHECK(wizard.result().accepted);
  HTMLPPT_CHECK_EQ(wizard.result().pairs.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(wizard.result().pairs[0].slide, 3);
  HTMLPPT_CHECK_EQ(wizard.result().pairs[0].title, std::string{"Three"});
  HTMLPPT_CHECK_EQ(wizard.result().pairs[0].html,
                   Utf8Path(dir.path() / "slide-3.html"));
  HTMLPPT_CHECK_EQ(wizard.result().html_folder, dir.path());
}

HTMLPPT_TEST_CASE(win32_wizard_window_show_returns_the_import_result) {
  const htmlppt::test::TempDir dir;
  MakeHtmlFile(dir.path() / "slide-1.html");

  WizardWindowOptions options;
  options.show = false;
  options.slides = {WizardSlide{1, "One"}};
  options.html_folder = dir.path();

  WizardWindow wizard;
  HTMLPPT_CHECK(wizard.Create(options));
  const HWND import_button = wizard.import_button();
  // Queue the click before the modal loop starts: Show processes it as its
  // first message, closes the wizard, and returns the accepted result. This
  // drives the same local loop a real toolbar click runs.
  HTMLPPT_CHECK(PostMessageW(
                    wizard.handle(), WM_COMMAND,
                    MAKEWPARAM(GetDlgCtrlID(import_button), BN_CLICKED),
                    reinterpret_cast<LPARAM>(import_button)) != 0);
  HTMLPPT_CHECK(wizard.Show());
  HTMLPPT_CHECK(wizard.handle() == nullptr);
  HTMLPPT_CHECK(wizard.result().accepted);
  HTMLPPT_CHECK_EQ(wizard.result().pairs.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(wizard.result().pairs[0].slide, 1);
}

HTMLPPT_TEST_CASE(win32_wizard_window_cancel_yields_no_pairs) {
  const htmlppt::test::TempDir dir;
  MakeHtmlFile(dir.path() / "slide-1.html");

  WizardWindowOptions options;
  options.show = false;
  options.slides = {WizardSlide{1, "One"}};
  options.html_folder = dir.path();

  WizardWindow wizard;
  HTMLPPT_CHECK(wizard.Create(options));
  HTMLPPT_CHECK_EQ(ListView_GetItemCount(wizard.list_view()), 1);
  const HWND cancel_button = wizard.cancel_button();
  Click(wizard.handle(), cancel_button);
  HTMLPPT_CHECK(wizard.handle() == nullptr);
  HTMLPPT_CHECK(!wizard.result().accepted);
  HTMLPPT_CHECK(wizard.result().pairs.empty());
}

HTMLPPT_TEST_CASE(win32_wizard_window_requires_one_checked_row_to_import) {
  const htmlppt::test::TempDir dir;
  MakeHtmlFile(dir.path() / "slide-1.html");

  WizardWindowOptions options;
  options.show = false;
  options.slides = {WizardSlide{1, "One"}};
  options.html_folder = dir.path();

  WizardWindow wizard;
  HTMLPPT_CHECK(wizard.Create(options));
  ListView_SetCheckState(wizard.list_view(), 0, FALSE);
  HTMLPPT_CHECK(wizard.checked_pairs().empty());
  Click(wizard.handle(), wizard.import_button());
  // An empty selection is not an import: the window stays open and says why.
  HTMLPPT_CHECK(wizard.handle() != nullptr);
  HTMLPPT_CHECK(!wizard.result().accepted);
  HTMLPPT_CHECK_EQ(StatusText(wizard.status_view()),
                   std::wstring(L"check at least one row to import"));
}

HTMLPPT_TEST_CASE(win32_wizard_window_reproposes_when_the_mode_changes) {
  const htmlppt::test::TempDir dir;
  MakeHtmlFile(dir.path() / "a.html");
  MakeHtmlFile(dir.path() / "b.html");

  WizardWindowOptions options;
  options.show = false;
  options.slides = {WizardSlide{1, "One"}, WizardSlide{2, "Two"}};
  options.html_folder = dir.path();

  WizardWindow wizard;
  HTMLPPT_CHECK(wizard.Create(options));
  // By file name finds no number in a.html/b.html, so the list starts empty.
  HTMLPPT_CHECK_EQ(ListView_GetItemCount(wizard.list_view()), 0);
  HTMLPPT_CHECK(!StatusText(wizard.status_view()).empty());

  SendMessageW(wizard.order_radio(), BM_CLICK, 0, 0);
  HTMLPPT_CHECK_EQ(ListView_GetItemCount(wizard.list_view()), 2);
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 0, 2),
                   Utf8Path(dir.path() / "a.html"));
  HTMLPPT_CHECK_EQ(ItemTextUtf8(wizard.list_view(), 1, 2),
                   Utf8Path(dir.path() / "b.html"));

  SendMessageW(wizard.filename_radio(), BM_CLICK, 0, 0);
  HTMLPPT_CHECK_EQ(ListView_GetItemCount(wizard.list_view()), 0);
}
#endif
