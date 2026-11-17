#pragma once

#include <cstdint>
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
  // Absolute normalized filesystem path without query/hash, encoded as
  // UTF-8. Callers must use PathFromUtf8 before filesystem operations.
  std::string html_path;
  std::string query;      // merged query without the leading '?'
  std::string fragment;   // merged fragment without the leading '#'
  std::string stage_name;
  std::string edge_profile;
  // Asset staging mode from the config: "direct" opens the source HTML in
  // place, "file" copies just the HTML, and "tree" copies the HTML plus its
  // asset root. assets_root is an absolute UTF-8 path, tree mode only.
  std::string assets_mode = "direct";
  std::string assets_root;
  // 0 means no cap. From assets.maxBytes.
  std::int64_t assets_max_bytes = 0;
  // tree-mode filters from assets.include / assets.exclude. A non-empty include
  // whitelists; exclude always wins.
  std::vector<std::string> assets_include;
  std::vector<std::string> assets_exclude;
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
