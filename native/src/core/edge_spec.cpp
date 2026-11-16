#include "core/edge_spec.h"

#include <algorithm>
#include <cctype>

namespace htmlppt {
namespace {

bool IsSafePathByte(unsigned char value) {
  return std::isalnum(value) != 0 || value == '/' || value == ':' ||
         value == '-' || value == '.' || value == '_' || value == '~';
}

std::string PercentEncodePath(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  std::string encoded;
  encoded.reserve(text.size() * 3);
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char value : text) {
    if (IsSafePathByte(value)) {
      encoded.push_back(static_cast<char>(value));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[value >> 4]);
      encoded.push_back(kHex[value & 0x0F]);
    }
  }
  return encoded;
}

} // namespace

std::string BuildEdgeUrl(const ResolvedTarget& target) {
  std::string path = target.html_path;
  std::replace(path.begin(), path.end(), '\\', '/');
  std::string url;
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    url = "file:///" + PercentEncodePath(path);
  } else if (!path.empty() && path.front() == '/') {
    url = "file://" + PercentEncodePath(path);
  } else {
    url = "file:///" + PercentEncodePath(path);
  }
  if (!target.query.empty()) url += "?" + target.query;
  if (!target.fragment.empty()) url += "#" + target.fragment;
  return url;
}

std::vector<std::string> DefaultEdgeFlags() {
  return {
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-sync",
      "--disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,"
      "msEdgeFirstRunExperience,EdgeSync",
      "--disable-background-networking",
      "--disable-component-update",
      "--disable-extensions",
      "--disable-default-apps",
      "--no-service-autorun",
      "--disable-client-side-phishing-detection",
      "--disable-domain-reliability",
      "--no-pings",
      "--disable-breakpad",
  };
}

EdgeLaunchSpec MakeEdgeLaunchSpec(const std::filesystem::path& edge_exe,
                                  const std::filesystem::path& temp_dir,
                                  const ResolvedTarget& target,
                                  const EdgeConfig& config) {
  EdgeLaunchSpec spec;
  spec.executable = edge_exe;
  spec.url = BuildEdgeUrl(target);
  spec.profile_dir = temp_dir / target.edge_profile;
  spec.arguments = config.args.empty() ? DefaultEdgeFlags() : config.args;
  spec.arguments.push_back("--app=" + spec.url);
  spec.arguments.push_back("--start-fullscreen");
  spec.arguments.push_back("--user-data-dir=" + spec.profile_dir.string());
  return spec;
}

} // namespace htmlppt
