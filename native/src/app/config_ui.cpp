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

  // Recomputes the rows and the Save gate after every action, and returns the
  // first blocking message: a target row's error when there is one, otherwise
  // the config-wide resolver error that has no row of its own (a missing or
  // invalid deck). Empty means the config is clean.
  const auto refresh = [&window, &model, deck_info]() -> std::wstring {
    const std::vector<DisplayRow> rows =
        model.DisplayRows(deck_info, ResolveOptions{});
    window.SetRows(rows);
    const bool blocked = model.HasBlockingError(deck_info, ResolveOptions{});
    window.SetSaveEnabled(model.dirty() && !blocked);
    if (!blocked) return {};
    for (const auto& row : rows) {
      if (row.state == "error") return Widen(row.issue);
    }
    for (const auto& issue : model.Validate(deck_info, ResolveOptions{})) {
      if (issue.severity == IssueSeverity::Error) return Widen(issue.message);
    }
    return L"configuration is invalid";
  };

  // The view reports the selection as a model index, never as a list position.
  window.SetSelectionHandler([&selected_index](int model_index) {
    if (model_index < 0) {
      selected_index.reset();
    } else {
      selected_index = static_cast<std::size_t>(model_index);
    }
  });

  window.SetEditHandler(
      [&model, &selected_index,
       refresh](const DetailFields& fields, std::wstring* error) {
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
        // rows and the Save gate here. ApplyDetail reports "applied" itself.
        refresh();
        return true;
      });

  // Toolbar actions: each runs one model call, then hands the fresh rows and
  // the Save gate back to the view. Add offers a placeholder html because an
  // empty one is rejected by the model, which would leave nothing to edit.
  window.SetAddHandler([&window, &model, refresh](std::wstring* error) {
    int max_slide = 0;
    for (const auto& row : model.rows()) {
      if (row.slide > max_slide) max_slide = row.slide;
    }
    std::string model_error;
    if (!model.AddTarget(max_slide + 1, "page.html", &model_error)) {
      if (error != nullptr) *error = Widen(model_error);
      return false;
    }
    const std::wstring reason = refresh();
    window.SetStatus(reason.empty() ? L"added target" : reason);
    return true;
  });

  window.SetRemoveHandler(
      [&window, &model, refresh](std::size_t index, std::wstring* error) {
        std::string model_error;
        if (!model.RemoveTarget(index, &model_error)) {
          if (error != nullptr) *error = Widen(model_error);
          return false;
        }
        const std::wstring reason = refresh();
        window.SetStatus(reason.empty() ? L"removed target" : reason);
        return true;
      });

  window.SetValidateHandler([&window, refresh](std::wstring* error) {
    // refresh() itself reports the first blocking issue, which is what the
    // status line must show when Save is closed by a global (non-row) error.
    const std::wstring reason = refresh();
    if (!reason.empty()) {
      if (error != nullptr) *error = reason;
      return false;
    }
    window.SetStatus(L"valid");
    return true;
  });

  window.SetSaveHandler([&model, config_path, refresh](std::wstring* error) {
    std::string save_error;
    if (!model.SaveWithBackup(config_path, &save_error)) {
      if (error != nullptr) *error = Widen(save_error);
      return false;
    }
    // A successful save clears dirty, so the gate closes until the next edit;
    // the view reports "saved" itself.
    refresh();
    return true;
  });

  window.SetRows(rows);
  window.SetStatus(Widen(ConfigStatusText(loaded, rows, config_path)));
  // Nothing is dirty before the first edit, so Save starts closed.
  window.SetSaveEnabled(false);
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
