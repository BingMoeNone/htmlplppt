#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/resolve.h"

namespace htmlppt {

struct EdgeLaunchSpec {
  std::filesystem::path executable;
  std::string url;
  std::filesystem::path profile_dir;
  std::vector<std::string> arguments;
};

std::string BuildEdgeUrl(const ResolvedTarget& target);
std::vector<std::string> DefaultEdgeFlags();
EdgeLaunchSpec MakeEdgeLaunchSpec(const std::filesystem::path& edge_exe,
                                  const std::filesystem::path& temp_dir,
                                  const ResolvedTarget& target,
                                  const EdgeConfig& config);

} // namespace htmlppt
