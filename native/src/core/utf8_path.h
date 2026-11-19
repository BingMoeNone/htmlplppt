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

// The config path made absolute against `current_dir`. Unlike ConfigDirFor this
// keeps the filename, so diagnostics can reopen the exact file a run used even
// when the run recorded the path before changing its working directory.
std::filesystem::path ResolveConfigPath(const std::filesystem::path& config_path,
                                        const std::filesystem::path& current_dir);

// Lexical equality after absolute()/lexically_normal(), case-insensitive on
// Windows (a case-insensitive filesystem would otherwise let Audit.html and
// audit.html be treated as two different files).
bool PathEquals(const std::filesystem::path& left,
                const std::filesystem::path& right);

// True when `child` is strictly below `parent` (equal paths are not inside).
// Uses the same normalized, case-insensitive-on-Windows comparison as
// PathEquals so a package directory is not mistaken for a sibling on a
// case-insensitive or differently-cased path.
bool PathIsInside(const std::filesystem::path& child,
                  const std::filesystem::path& parent);

// Writes `text` to `path` (or its sibling temp file first) and replaces the
// target in one rename. A failed write leaves the previous file intact, so a
// truncated worker-plan.json can never poison the next worker restart. The
// temp file lives in the same directory as the target so the rename is atomic.
bool WriteFileAtomically(const std::filesystem::path& path,
                         std::string_view text, std::string* error);

} // namespace htmlppt
