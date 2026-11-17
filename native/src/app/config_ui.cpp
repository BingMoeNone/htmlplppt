#include "app/config_ui.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#include "core/config.h"
#include "core/config_editor.h"
#include "core/deck_info.h"
#include "platform/win32/config_window.h"

namespace htmlppt {
namespace {

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

int RunConfigUi(const std::filesystem::path& config_path,
                const std::filesystem::path& deck_info_path) {
  // A missing or broken config is not fatal for the editor: the window opens
  // with an empty model and the loader's first issue in the status line, so the
  // user can repair the file instead of losing the window.
  const ConfigLoadResult loaded = LoadConfigFile(config_path);
  ConfigEditorModel model(loaded.config.has_value() ? *loaded.config : Config{});

  DeckInfoResult deck;
  if (!deck_info_path.empty()) {
    JsonDeckInfoProvider provider(deck_info_path);
    // Match the CLI: the deck path comes from the loaded config, not from the
    // config file itself. A config that failed to load has nothing to resolve.
    deck = provider.Load(loaded.config.has_value()
                             ? ResolveDeckPath(*loaded.config)
                             : std::filesystem::path{});
  }
  const std::vector<DisplayRow> rows = model.DisplayRows(
      deck.info.has_value() ? &*deck.info : nullptr, ResolveOptions{});

  ConfigWindow window;
  ConfigWindowOptions options;
  options.title = L"htmlppt config - " + config_path.wstring();
  if (!window.Create(options)) {
    std::cerr << "error: cannot create the config window\n";
    return 1;
  }
  window.SetRows(rows);

  std::wstring status;
  if (!loaded.config.has_value()) {
    status = L"cannot load " + config_path.wstring();
    if (!loaded.issues.empty()) {
      status += L": " + Widen(loaded.issues.front().message);
    }
  } else {
    status = std::to_wstring(rows.size()) + L" target(s)";
    for (const auto& row : rows) {
      if (row.state == "error") {
        status += L" - " + Widen(row.issue);
        break;
      }
    }
  }
  window.SetStatus(status);
  return window.RunMessageLoop();
}

} // namespace htmlppt

#else

namespace htmlppt {

int RunConfigUi(const std::filesystem::path&, const std::filesystem::path&) {
  std::cerr << "error: --config-ui requires Windows\n";
  return 2;
}

} // namespace htmlppt

#endif // _WIN32
