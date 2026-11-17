#pragma once

#include <filesystem>

namespace htmlppt {

// Everything RunConfigUi needs from the command line. `allow_outside_folder`
// mirrors --allow-outside-folder so the editor validates exactly like a run.
struct ConfigUiOptions {
  std::filesystem::path config_path;
  std::filesystem::path deck_info_path;
  bool allow_outside_folder = false;
};

// Opens the config editor for one watcher.json. Returns the process exit code.
// deck_info_path is the optional --deck-info JSON used for validation; without
// it Windows falls back to WPS, and other platforms validate without deck info.
int RunConfigUi(const ConfigUiOptions& options);

} // namespace htmlppt
