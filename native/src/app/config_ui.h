#pragma once

#include <filesystem>

namespace htmlppt {

// Opens the config editor for one watcher.json. Returns the process exit code.
// deck_info_path is the optional --deck-info JSON used for validation.
int RunConfigUi(const std::filesystem::path& config_path,
                const std::filesystem::path& deck_info_path);

} // namespace htmlppt
