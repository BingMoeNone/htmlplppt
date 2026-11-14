#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/deck_info.h"

namespace htmlppt {

struct ResolveOptions {
  bool allow_outside_folder = false;
  // When empty, ResolveTargets computes the fingerprint of the deck path.
  std::string deck_fingerprint;
};

enum class ResolveState {
  Resolved,
  Disabled,
};

struct ResolvedTarget {
  int ordinal = 0;  // 1-based position in Config::active.targets
  ResolveState state = ResolveState::Resolved;
  int slide = 0;  // 0 when the target is disabled
  std::string label;
  std::string html_path;  // absolute normalized filesystem path, no query/hash
  std::string query;      // merged query without the leading '?'
  std::string fragment;   // merged fragment without the leading '#'
  std::string stage_name;
  std::string edge_profile;
};

struct ResolveResult {
  std::vector<ResolvedTarget> targets;  // includes disabled targets
  std::vector<ConfigIssue> issues;
};

std::filesystem::path ConfigBaseDir(const Config& config);
std::filesystem::path ResolveDeckPath(const Config& config);
ResolveResult ResolveTargets(const Config& config, const DeckInfo* deck,
                             const ResolveOptions& options = {});
std::string ResolveTableToText(const ResolveResult& result);

} // namespace htmlppt
