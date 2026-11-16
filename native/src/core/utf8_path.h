#pragma once

#include <filesystem>
#include <string>

namespace htmlppt {

// Returns the path as UTF-8 bytes, independent of the platform's narrow
// encoding. Callers that need a narrow string for serialization or argv
// conversion should use this helper instead of path::string().
std::string Utf8Path(const std::filesystem::path& path);

} // namespace htmlppt
