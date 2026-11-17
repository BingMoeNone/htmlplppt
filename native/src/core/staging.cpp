#include "core/staging.h"

#include <system_error>
#include <utility>

#include "core/utf8_path.h"

namespace htmlppt {
namespace {

std::uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

bool CopyFile(const std::filesystem::path& from,
              const std::filesystem::path& to, std::string* error) {
  std::error_code code;
  std::filesystem::create_directories(to.parent_path(), code);
  std::filesystem::copy_file(from, to,
                             std::filesystem::copy_options::overwrite_existing,
                             code);
  if (code) {
    *error = "cannot copy " + Utf8Path(from) + " to " + Utf8Path(to) + ": " +
             code.message();
    return false;
  }
  return true;
}

// Glob match for the tree-mode filters: '*' (and '**') match any run including
// '/', '?' matches one character. Iterative backtracking keeps it cheap.
bool GlobMatch(std::string_view pattern, std::string_view text) {
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star = std::string_view::npos;
  std::size_t mark = 0;
  while (text_index < text.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' ||
         pattern[pattern_index] == text[text_index])) {
      ++pattern_index;
      ++text_index;
    } else if (pattern_index < pattern.size() &&
               pattern[pattern_index] == '*') {
      star = pattern_index++;
      mark = text_index;
    } else if (star != std::string_view::npos) {
      pattern_index = star + 1;
      text_index = ++mark;
    } else {
      return false;
    }
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

// Exclude always wins; an empty include whitelists everything.
bool PassesFilters(std::string_view relative,
                   const std::vector<std::string>& include,
                   const std::vector<std::string>& exclude) {
  for (const auto& pattern : exclude) {
    if (GlobMatch(pattern, relative)) return false;
  }
  if (include.empty()) return true;
  for (const auto& pattern : include) {
    if (GlobMatch(pattern, relative)) return true;
  }
  return false;
}

} // namespace

StageOutcome StageTargetHtml(const ResolvedTarget& target,
                             const std::filesystem::path& stage_dir,
                             std::int64_t max_bytes) {
  StageOutcome outcome;
  const auto source = PathFromUtf8(target.html_path);
  outcome.html_path = source;

  // "direct" (and any unknown mode, which config validation rejects before we
  // get here) opens the source file in place.
  if (target.assets_mode != "file" && target.assets_mode != "tree") {
    outcome.staged = true;
    return outcome;
  }

  if (target.stage_name.empty()) {
    outcome.error = "stageName is required for assets mode " +
                    target.assets_mode;
    return outcome;
  }
  std::error_code code;
  if (!std::filesystem::is_regular_file(source, code)) {
    outcome.error = "HTML file does not exist: " + target.html_path;
    return outcome;
  }

  const bool tree = target.assets_mode == "tree";
  const auto staged_html = stage_dir / PathFromUtf8(target.stage_name);
  std::filesystem::create_directories(stage_dir, code);
  if (code) {
    outcome.error = "cannot create stage directory " + Utf8Path(stage_dir) +
                    ": " + code.message();
    return outcome;
  }

  const auto html_bytes = FileSize(source);
  if (max_bytes > 0 && html_bytes > static_cast<std::uint64_t>(max_bytes)) {
    outcome.error = "staged HTML exceeds assets.maxBytes: " +
                    std::to_string(html_bytes) + " > " +
                    std::to_string(max_bytes);
    return outcome;
  }
  std::string error;
  if (!CopyFile(source, staged_html, &error)) {
    outcome.error = std::move(error);
    return outcome;
  }
  std::uint64_t copied = html_bytes;

  if (tree) {
    const auto root = PathFromUtf8(target.assets_root);
    if (root.empty() || !std::filesystem::is_directory(root, code)) {
      outcome.error = "asset root does not exist: " + target.assets_root;
      return outcome;
    }
    std::filesystem::recursive_directory_iterator it(root, code);
    if (code) {
      outcome.error = "cannot read asset root " + target.assets_root + ": " +
                      code.message();
      return outcome;
    }
    for (const auto& entry : it) {
      if (!entry.is_regular_file(code)) continue;
      if (entry.path() == source) continue;  // already staged as stage_name
      const auto relative = std::filesystem::relative(entry.path(), root, code);
      if (code) continue;
      const std::string relative_text = relative.generic_string();
      if (!PassesFilters(relative_text, target.assets_include,
                         target.assets_exclude)) {
        continue;
      }
      const auto bytes = FileSize(entry.path());
      if (max_bytes > 0 &&
          copied + bytes > static_cast<std::uint64_t>(max_bytes)) {
        outcome.error = "staged assets exceed assets.maxBytes at " +
                        Utf8Path(relative);
        return outcome;
      }
      std::string copy_error;
      if (!CopyFile(entry.path(), stage_dir / relative, &copy_error)) {
        outcome.error = std::move(copy_error);
        return outcome;
      }
      copied += bytes;
    }
  }

  outcome.html_path = staged_html;
  outcome.copied_bytes = copied;
  outcome.staged = true;
  return outcome;
}

} // namespace htmlppt
