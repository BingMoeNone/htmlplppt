#include "app/config_ui.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#include "core/config.h"
#include "core/config_editor.h"
#include "core/deck_info.h"
#include "core/resolve.h"
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
  const DeckInfo* deck_info = deck.info.has_value() ? &*deck.info : nullptr;
  const std::vector<DisplayRow> rows =
      model.DisplayRows(deck_info, ResolveOptions{});

  // Declared before the window so it outlives the handlers stored in it.
  std::optional<std::size_t> selected_index;

  ConfigWindow window;
  ConfigWindowOptions options;
  options.title = L"htmlppt config - " + config_path.wstring();
  if (!window.Create(options)) {
    std::cerr << "error: cannot create the config window\n";
    return 1;
  }

  // The view reports the selected row's model index, never its own row
  // position; the edit handler applies to exactly that target.
  window.SetSelectionHandler([&selected_index](int model_index) {
    if (model_index < 0) {
      selected_index.reset();
    } else {
      selected_index = static_cast<std::size_t>(model_index);
    }
  });

  window.SetEditHandler(
      [&window, &model, &selected_index,
       deck_info](const DetailFields& fields, std::wstring* error) {
        TargetEdit edit;
        std::wstring parse_error;
        if (!TargetEditFromDetailFields(fields, &edit, &parse_error)) {
          if (error != nullptr) *error = parse_error;
          return false;
        }
        if (!selected_index.has_value()) {
          if (error != nullptr) *error = L"no row selected";
          return false;
        }
        std::string model_error;
        if (!model.ApplyEdit(*selected_index, edit, &model_error)) {
          if (error != nullptr) *error = Widen(model_error);
          return false;
        }
        // The view never writes Config: after a successful edit, recompute the
        // rows here and hand the fresh snapshot to the window.
        window.SetRows(model.DisplayRows(deck_info, ResolveOptions{}));
        window.SetStatus(L"applied");
        return true;
      });

  window.SetRows(rows);
  window.SetStatus(Widen(ConfigStatusText(loaded, rows, config_path)));
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
