#pragma once

#ifdef _WIN32

#include <windows.h>

#include <commctrl.h>

#include <filesystem>
#include <string>
#include <vector>

#include "core/mapping_wizard.h"

namespace htmlppt {

struct WizardWindowOptions {
  std::wstring title = L"htmlppt mapping wizard";
  int width = 780;
  int height = 520;
  // Tests create the window hidden so a headless run never flashes a window.
  // Show() displays it either way and runs the modal loop.
  bool show = true;
  // The editor window, disabled while the wizard is open. Null in tests.
  HWND owner = nullptr;
  // The deck to propose targets for, and the folder to scan on open.
  std::vector<WizardSlide> slides;
  std::filesystem::path html_folder;
};

// What the wizard was confirmed with. `accepted` is false on Cancel/close, in
// which case the app must not touch the model.
struct WizardResult {
  bool accepted = false;
  std::vector<WizardPair> pairs;
  std::filesystem::path html_folder;
};

// The auto-mapping wizard (§9.3): lists the deck's slides and the HTML files of
// a chosen folder, proposes pairs by file-name number or file order, lets the
// user uncheck rows, and returns the checked pairs for the app to import. The
// view never touches Config or ConfigEditorModel.
class WizardWindow {
 public:
  WizardWindow() = default;
  WizardWindow(const WizardWindow&) = delete;
  WizardWindow& operator=(const WizardWindow&) = delete;
  ~WizardWindow();

  bool Create(const WizardWindowOptions& options);
  // Shows the window and runs a local modal loop until Import, Cancel, or
  // close. Returns result().accepted; the window is destroyed on return.
  bool Show();
  // Posts WM_CLOSE to the open window.
  bool Close();

  HWND handle() const { return handle_; }
  HWND list_view() const { return list_view_; }
  HWND status_view() const { return status_view_; }
  HWND choose_folder_button() const { return choose_folder_button_; }
  HWND filename_radio() const { return filename_radio_; }
  HWND order_radio() const { return order_radio_; }
  HWND import_button() const { return import_button_; }
  HWND cancel_button() const { return cancel_button_; }

  // The current proposals, in deck order.
  const std::vector<WizardPair>& pairs() const { return pairs_; }
  // The live checked rows: the same subset Import accepts.
  std::vector<WizardPair> checked_pairs() const;
  const WizardResult& result() const { return result_; }
  std::filesystem::path html_folder() const { return html_folder_; }
  void SetStatus(const std::wstring& text);
  // Test seam: replace the proposals without going through a folder scan. The
  // list repopulates and every row starts checked as the pairs say.
  void SetPairsForTest(const std::vector<WizardPair>& pairs);

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void CreateChildren();
  void CreateFontForDpi();
  void ApplyFont();
  int Scale(int value) const;
  void Layout(int width, int height);
  // Enumerates the folder's regular files (sorted) and re-proposes with the
  // current mode; the folder becomes the wizard's HTML root.
  void ScanFolder(const std::filesystem::path& folder);
  // Re-runs ProposePairs over the last scan's files.
  void RecomputePairs();
  void FillList();
  void ChooseFolder();
  void Import();
  void Cancel();

  HWND owner_ = nullptr;
  HWND handle_ = nullptr;
  HWND list_view_ = nullptr;
  HWND status_view_ = nullptr;
  HWND choose_folder_button_ = nullptr;
  HWND filename_radio_ = nullptr;
  HWND order_radio_ = nullptr;
  HWND import_button_ = nullptr;
  HWND cancel_button_ = nullptr;
  HFONT font_ = nullptr;
  std::vector<WizardSlide> slides_;
  // The last scan's files, kept so a mode change re-proposes without reading
  // the disk again.
  std::vector<std::string> html_files_;
  std::vector<WizardPair> pairs_;
  PairingMode mode_ = PairingMode::ByFileName;
  std::filesystem::path html_folder_;
  WizardResult result_;
  bool done_ = false;
  UINT dpi_ = 96;
};

} // namespace htmlppt

#endif // _WIN32
