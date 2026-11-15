#pragma once

#include <cstdint>
#include <filesystem>

namespace htmlppt {

bool WriteMiniDump(std::uint32_t pid, const std::filesystem::path& path);

} // namespace htmlppt
