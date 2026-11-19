#include "core/resolve.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include "core/config_issue.h"
#include "core/utf8_path.h"

namespace htmlppt {
namespace {

std::filesystem::path NormalizePath(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (!error) return normalized;
  error.clear();
  auto absolute = std::filesystem::absolute(path, error);
  if (!error) return absolute.lexically_normal();
  return path;
}

bool IsWithin(const std::filesystem::path& base,
              const std::filesystem::path& candidate) {
  auto base_it = base.begin();
  auto candidate_it = candidate.begin();
  for (; base_it != base.end(); ++base_it, ++candidate_it) {
    if (candidate_it == candidate.end()) return false;
    if (*base_it != *candidate_it) return false;
  }
  return true;
}

struct HtmlReference {
  std::string path;
  std::string query;
  std::string fragment;
};

HtmlReference SplitHtmlReference(const std::string& html) {
  HtmlReference result;
  std::string remainder = html;
  if (const auto hash = remainder.find('#'); hash != std::string::npos) {
    result.fragment = remainder.substr(hash + 1);
    remainder.erase(hash);
  }
  if (const auto question = remainder.find('?'); question != std::string::npos) {
    result.query = remainder.substr(question + 1);
    remainder.erase(question);
  }
  result.path = remainder;
  return result;
}

struct QueryFragment {
  std::string query;
  std::string fragment;
};

QueryFragment ParseExtraReference(const std::string& text) {
  QueryFragment result;
  std::string remainder = text;
  if (!remainder.empty() && remainder.front() == '?') remainder.erase(0, 1);
  if (const auto hash = remainder.find('#'); hash != std::string::npos) {
    result.fragment = remainder.substr(hash + 1);
    remainder.erase(hash);
  }
  result.query = remainder;
  return result;
}

std::vector<std::string> SplitQuery(const std::string& query) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto end = query.find('&', start);
    const auto part =
        query.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
    if (!part.empty()) parts.push_back(part);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}

std::string QueryKey(const std::string& part) {
  const auto equals = part.find('=');
  return equals == std::string::npos ? part : part.substr(0, equals);
}

bool MergeQuery(const std::string& first, const std::string& second,
                std::string* merged, std::string* conflict) {
  std::vector<std::string> parts = SplitQuery(first);
  std::map<std::string, std::set<std::string>> first_values;
  for (const auto& part : parts) {
    first_values[QueryKey(part)].insert(part);
  }
  std::set<std::string> seen(parts.begin(), parts.end());
  for (const auto& part : SplitQuery(second)) {
    const auto key = QueryKey(part);
    const auto found = first_values.find(key);
    if (found != first_values.end() && found->second.count(part) == 0) {
      *conflict = key;
      return false;
    }
    if (seen.insert(part).second) parts.push_back(part);
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out << '&';
    out << parts[i];
  }
  *merged = out.str();
  return true;
}

std::string AsciiSlug(const std::string& text) {
  std::string slug;
  for (unsigned char character : text) {
    if (character >= 'A' && character <= 'Z') {
      slug.push_back(static_cast<char>(character - 'A' + 'a'));
    } else if ((character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9')) {
      slug.push_back(static_cast<char>(character));
    } else if (!slug.empty() && slug.back() != '-') {
      slug.push_back('-');
    }
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  return slug.empty() ? "page" : slug;
}

std::string AutoStageName(int ordinal, const std::string& html_path) {
  const std::string stem = Utf8Path(PathFromUtf8(html_path).stem());
  std::ostringstream out;
  out << "target-" << std::setw(2) << std::setfill('0') << ordinal << "-"
      << AsciiSlug(stem) << ".html";
  return out.str();
}

std::string AutoEdgeProfile(const std::string& fingerprint, int ordinal) {
  std::ostringstream out;
  out << "htmlppt-" << (fingerprint.empty() ? "nodeck" : fingerprint) << "-t"
      << std::setw(2) << std::setfill('0') << ordinal;
  return out.str();
}

bool ValidateStageName(const std::string& value, const std::string& target_path,
                       std::vector<ConfigIssue>& issues) {
  if (value.empty()) return true;
  if (value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos) {
    AddError(issues, target_path + ".stageName",
             "must not contain path separators");
    return false;
  }
  if (value == "." || value == "..") {
    AddError(issues, target_path + ".stageName", "must be a file name");
    return false;
  }
  for (unsigned char character : value) {
    if (character > 0x7F) {
      AddError(issues, target_path + ".stageName", "must be ASCII");
      return false;
    }
  }
  return true;
}

bool ValidateEdgeProfile(const std::string& value,
                         const std::string& target_path,
                         std::vector<ConfigIssue>& issues) {
  if (value.empty()) {
    AddError(issues, target_path + ".edgeProfile", "must not be empty");
    return false;
  }
  if (value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos) {
    AddError(issues, target_path + ".edgeProfile",
             "must not contain path separators");
    return false;
  }
  return true;
}

bool MatchesPattern(const std::string& value, const std::string& pattern,
                    const std::string& mode) {
  if (mode == "exact") return value == pattern;
  if (mode == "prefix") return value.rfind(pattern, 0) == 0;
  if (mode == "regex") {
    try {
      return std::regex_search(value, std::regex(pattern));
    } catch (const std::regex_error&) {
      return false;
    }
  }
  return false;
}

std::optional<int> ResolveSlideNumber(const TargetConfig& target,
                                      const DeckInfo* deck,
                                      const std::string& target_path,
                                      std::vector<ConfigIssue>& issues) {
  if (target.slide.has_value()) {
    if (deck != nullptr) {
      const int count = static_cast<int>(deck->slides.size());
      if (*target.slide > count) {
        AddError(issues, target_path + ".slide",
                 "slide " + std::to_string(*target.slide) +
                     " is out of range (deck has " + std::to_string(count) +
                     " slides)");
        return std::nullopt;
      }
    } else {
      AddWarning(issues, target_path + ".slide",
                 "deck info is unavailable; slide range was not validated");
    }
    return *target.slide;
  }
  if (!target.match.has_value()) {
    AddError(issues, target_path, "exactly one of slide or match is required");
    return std::nullopt;
  }
  if (deck == nullptr) {
    AddError(issues, target_path + ".match",
             "deck info is required to resolve a slide title or name");
    return std::nullopt;
  }
  const MatchConfig& match = *target.match;
  const bool use_title = !match.slide_title.empty();
  const std::string& pattern =
      use_title ? match.slide_title : match.slide_name;
  const std::string field = use_title ? "slideTitle" : "slideName";
  if (match.mode == "regex") {
    try {
      std::regex compiled(pattern);
      (void)compiled;
    } catch (const std::regex_error& error) {
      AddError(issues, target_path + ".match.mode",
               std::string("invalid regex: ") + error.what());
      return std::nullopt;
    }
  }
  std::vector<int> matches;
  for (const auto& slide : deck->slides) {
    const std::string& value = use_title ? slide.title : slide.name;
    if (MatchesPattern(value, pattern, match.mode)) {
      matches.push_back(slide.index);
    }
  }
  if (matches.empty()) {
    AddError(issues, target_path + ".match." + field,
             "no slide matches " + match.mode + " pattern '" + pattern + "'");
    return std::nullopt;
  }
  if (matches.size() > 1) {
    std::ostringstream list;
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (i != 0) list << ", ";
      list << matches[i];
    }
    AddError(issues, target_path + ".match." + field,
             "pattern matches multiple slides: " + list.str());
    return std::nullopt;
  }
  return matches.front();
}

bool ResolveHtml(const TargetConfig& target, const std::filesystem::path& base_dir,
                 bool allow_outside_folder, std::size_t index,
                 ResolvedTarget* resolved, std::vector<ConfigIssue>& issues) {
  const std::string target_path = "targets[" + std::to_string(index) + "]";
  const HtmlReference html = SplitHtmlReference(target.html);
  if (html.path.empty()) {
    AddError(issues, target_path + ".html", "HTML path must not be empty");
    return false;
  }
  std::filesystem::path html_path = PathFromUtf8(html.path);
  if (html_path.is_relative()) html_path = base_dir / html_path;
  html_path = NormalizePath(html_path);
  if (!allow_outside_folder && !IsWithin(base_dir, html_path)) {
    AddError(issues, target_path + ".html",
             "HTML path is outside the config folder; use "
             "--allow-outside-folder to override");
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(html_path, error)) {
    AddError(issues, target_path + ".html",
             "HTML file does not exist: " + Utf8Path(html_path));
    return false;
  }
  const QueryFragment extra = ParseExtraReference(target.html_query);
  std::string query;
  std::string conflict;
  if (!MergeQuery(html.query, extra.query, &query, &conflict)) {
    AddError(issues, target_path + ".htmlQuery",
             "query parameter conflicts with html: " + conflict);
    return false;
  }
  if (!html.fragment.empty() && !extra.fragment.empty() &&
      html.fragment != extra.fragment) {
    AddError(issues, target_path + ".htmlQuery",
             "fragment conflicts with html: " + extra.fragment);
    return false;
  }
  resolved->html_path = Utf8Path(html_path);
  resolved->query = query;
  resolved->fragment =
      !html.fragment.empty() ? html.fragment : extra.fragment;
  return true;
}

bool ResolveAssets(const std::string& mode, const std::string& root_text,
                   const std::filesystem::path& base_dir,
                   const std::filesystem::path& html_path, std::size_t index,
                   bool allow_outside_folder,
                   std::vector<ConfigIssue>& issues,
                   std::string* resolved_root) {
  if (mode != "tree") return true;
  const std::string target_path = "targets[" + std::to_string(index) + "]";
  std::filesystem::path root = root_text.empty()
                                   ? html_path.parent_path()
                                   : PathFromUtf8(root_text);
  if (root.is_relative()) root = base_dir / root;
  root = NormalizePath(root);
  if (!allow_outside_folder && !IsWithin(base_dir, root)) {
    AddError(issues, target_path + ".assets.root",
             "asset root is outside the config folder; use "
             "--allow-outside-folder to override");
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    AddError(issues, target_path + ".assets.root",
             "asset root does not exist: " + Utf8Path(root));
    return false;
  }
  if (resolved_root != nullptr) *resolved_root = Utf8Path(root);
  return true;
}

ResolvedTarget DisabledTarget(int ordinal, const TargetConfig& target) {
  ResolvedTarget resolved;
  resolved.ordinal = ordinal;
  resolved.state = ResolveState::Disabled;
  resolved.label = target.label;
  return resolved;
}

} // namespace

std::filesystem::path ConfigBaseDir(const Config& config) {
  std::error_code error;
  if (config.source_path.empty()) {
    return std::filesystem::current_path(error).lexically_normal();
  }
  const auto parent = config.source_path.parent_path();
  if (parent.empty()) {
    return std::filesystem::current_path(error).lexically_normal();
  }
  return NormalizePath(parent);
}

std::filesystem::path ResolveDeckPath(const Config& config) {
  if (config.active.deck.empty()) return {};
  std::filesystem::path deck = PathFromUtf8(config.active.deck);
  if (deck.is_relative()) deck = ConfigBaseDir(config) / deck;
  return NormalizePath(deck);
}

ResolveResult ResolveTargets(const Config& config, const DeckInfo* deck,
                             const ResolveOptions& options) {
  ResolveResult result;
  const auto base_dir = ConfigBaseDir(config);
  const auto deck_path = ResolveDeckPath(config);
  if (config.active.deck.empty()) {
    AddError(result.issues, "deck", "must not be empty");
  } else {
    std::error_code error;
    if (!std::filesystem::is_regular_file(deck_path, error)) {
      AddError(result.issues, "deck",
               "deck file does not exist: " + Utf8Path(deck_path));
    }
  }
  if (config.active.targets.empty()) {
    AddWarning(result.issues, "targets", "no targets are configured");
  }

  std::string fingerprint = options.deck_fingerprint;
  if (fingerprint.empty() && !deck_path.empty()) {
    fingerprint = ComputeDeckFingerprint(deck_path);
  }
  if (fingerprint.empty()) {
    for (const auto& target : config.active.targets) {
      if (target.enabled && target.edge_profile.empty()) {
        AddWarning(result.issues, "deck",
                   "deck fingerprint is unavailable; auto Edge profiles are "
                   "not deck-specific");
        break;
      }
    }
  }

  std::map<int, int> slide_owner;
  std::map<std::string, int> stage_owner;
  std::map<std::string, int> profile_owner;

  for (std::size_t i = 0; i < config.active.targets.size(); ++i) {
    const TargetConfig& target = config.active.targets[i];
    const int ordinal = static_cast<int>(i) + 1;
    const std::string target_path = "targets[" + std::to_string(i) + "]";
    if (!target.enabled) {
      result.targets.push_back(DisabledTarget(ordinal, target));
      continue;
    }

    const auto slide =
        ResolveSlideNumber(target, deck, target_path, result.issues);
    if (!slide.has_value()) continue;

    ResolvedTarget resolved;
    resolved.ordinal = ordinal;
    resolved.slide = *slide;
    resolved.label = target.label.empty() ? "Slide " + std::to_string(*slide)
                                          : target.label;
    if (!ResolveHtml(target, base_dir, options.allow_outside_folder, i,
                     &resolved, result.issues)) {
      continue;
    }
    // A target without its own assets block inherits the global stage policy,
    // so --stage-mode and stage.mode in the config actually take effect.
    const std::string assets_mode =
        target.assets_set ? target.assets.mode : config.active.stage.mode;
    // A target with its own assets block but no root defaults to the HTML
    // directory (ResolveAssets uses html_path.parent_path()). The global
    // stage.root applies only to targets without an assets block.
    const std::string assets_root_text =
        target.assets_set ? target.assets.root : config.active.stage.root;
    std::string assets_root;
    if (!ResolveAssets(assets_mode, assets_root_text, base_dir,
                       PathFromUtf8(resolved.html_path), i,
                       options.allow_outside_folder, result.issues,
                       &assets_root)) {
      continue;
    }
    resolved.assets_mode = assets_mode;
    resolved.assets_root = std::move(assets_root);
    resolved.assets_max_bytes = target.assets.max_bytes;
    resolved.assets_include = target.assets.include;
    resolved.assets_exclude = target.assets.exclude;
    if (target.edge.has_value()) {
      // Carry the target's own Edge block into the plan; the worker previously
      // dropped both fields and always launched with the global config.
      resolved.edge_args = target.edge->args;
      resolved.edge_args_set = true;
      resolved.edge_prewarm = target.edge->prewarm_profile;
    }

    const std::string stage = target.stage_name.empty()
                                  ? AutoStageName(ordinal, resolved.html_path)
                                  : target.stage_name;
    if (!ValidateStageName(stage, target_path, result.issues)) continue;
    const std::string profile =
        target.edge_profile.empty() ? AutoEdgeProfile(fingerprint, ordinal)
                                    : target.edge_profile;
    if (!ValidateEdgeProfile(profile, target_path, result.issues)) continue;

    if (const auto owner = slide_owner.find(*slide); owner != slide_owner.end()) {
      AddError(result.issues,
               target_path + (target.slide.has_value() ? ".slide" : ".match"),
               "slide " + std::to_string(*slide) +
                   " is already mapped by target " +
                   std::to_string(owner->second));
      continue;
    }
    if (const auto owner = stage_owner.find(stage); owner != stage_owner.end()) {
      AddError(result.issues, target_path + ".stageName",
               "duplicate stage name '" + stage + "' (target " +
                   std::to_string(owner->second) + ")");
      continue;
    }
    if (const auto owner = profile_owner.find(profile);
        owner != profile_owner.end()) {
      AddError(result.issues, target_path + ".edgeProfile",
               "duplicate Edge profile '" + profile + "' (target " +
                   std::to_string(owner->second) + ")");
      continue;
    }

    slide_owner.emplace(*slide, ordinal);
    stage_owner.emplace(stage, ordinal);
    profile_owner.emplace(profile, ordinal);
    resolved.stage_name = stage;
    resolved.edge_profile = profile;
    resolved.state = ResolveState::Resolved;
    result.targets.push_back(std::move(resolved));
  }
  return result;
}

std::string ResolveTableToText(const ResolveResult& result) {
  std::vector<std::vector<std::string>> rows;
  rows.push_back({"#", "SLIDE", "LABEL", "HTML", "STAGE", "ASSETS",
                  "EDGE-PROFILE", "STATE"});
  for (const auto& target : result.targets) {
    rows.push_back(
        {std::to_string(target.ordinal),
         target.state == ResolveState::Disabled ? "-"
                                                : std::to_string(target.slide),
         target.label.empty() ? "-" : target.label,
         target.html_path.empty() ? "-" : target.html_path,
         target.stage_name.empty() ? "-" : target.stage_name,
         target.assets_mode.empty() ? "-" : target.assets_mode,
         target.edge_profile.empty() ? "-" : target.edge_profile,
         target.state == ResolveState::Disabled ? "disabled" : "resolved"});
  }
  std::vector<std::size_t> widths(rows.front().size(), 0);
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }
  std::ostringstream out;
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      out << std::left << std::setw(static_cast<int>(widths[i])) << row[i];
      if (i + 1 != row.size()) out << "  ";
    }
    out << '\n';
  }
  return out.str();
}
} // namespace htmlppt
