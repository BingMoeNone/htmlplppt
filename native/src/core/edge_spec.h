#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/resolve.h"

namespace htmlppt {

// Config strings are UTF-8. Convert them with PathFromUtf8 before using
// them as filesystem paths. Executable and temp directories are already
// native paths.
struct EdgeLaunchSpec {
  // Native path. Plan 5B-2 converts it to UTF-16 for CreateProcessW.
  std::filesystem::path executable;
  // UTF-8 URL.
  std::string url;
  // Native path. The --user-data-dir argument is serialized as UTF-8.
  std::filesystem::path profile_dir;
  // UTF-8 argument strings. Plan 5B-2 converts them to UTF-16 (not the
  // active code page) before calling CreateProcessW, so non-ASCII paths
  // survive intact.
  std::vector<std::string> arguments;
};

std::string BuildEdgeUrl(const ResolvedTarget& target);
std::vector<std::string> DefaultEdgeFlags();
// Prewarm spec: launches Edge once with the run's profile and an empty page so
// the browser initializes its user-data-dir before the demo needs it. The app
// kills it after a short bound; it never shows a window.
EdgeLaunchSpec MakeEdgePrewarmSpec(const std::filesystem::path& edge_exe,
                                   const std::filesystem::path& profile_dir,
                                   const EdgeConfig& config);
// target.edge_profile is a UTF-8 config string and is converted with
// PathFromUtf8. edge_exe and temp_dir are native paths.
EdgeLaunchSpec MakeEdgeLaunchSpec(const std::filesystem::path& edge_exe,
                                  const std::filesystem::path& temp_dir,
                                  const ResolvedTarget& target,
                                  const EdgeConfig& config);

} // namespace htmlppt
