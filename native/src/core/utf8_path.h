#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace htmlppt {

// Returns the path as UTF-8 bytes, independent of the platform's narrow
// encoding. Callers that need a narrow string for serialization or argv
// conversion should use this helper instead of path::string().
std::string Utf8Path(const std::filesystem::path& path);

// Builds a native path from UTF-8 bytes. On Windows the value is converted
// to UTF-16; it is never interpreted with the active code page. Callers
// should use this helper instead of constructing a path from a narrow
// config string.
std::filesystem::path PathFromUtf8(std::string_view text);

// Parent directory of a config path. A bare relative filename is resolved
// against `current_dir` first, so `--config watcher.json` still finds the
// legacy mirror next to the file instead of disabling it.
std::filesystem::path ConfigDirFor(const std::filesystem::path& config_path,
                                   const std::filesystem::path& current_dir);

} // namespace htmlppt
