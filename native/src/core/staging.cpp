#include "core/staging.h"

#include <system_error>
#include <utility>
#include <vector>

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
  // Every file this target put in the shared stage directory. A failure removes
  // them again: a later target must not serve (or collide with) a half-staged
  // tree that belongs to a target the resolver rejected.
  std::vector<std::filesystem::path> created;
  created.push_back(staged_html);
  const auto abort_staging = [&](std::string message) {
    for (const auto& path : created) {
      std::error_code cleanup;
      std::filesystem::remove(path, cleanup);
    }
    outcome.error = std::move(message);
    return outcome;
  };
  if (!CopyFile(source, staged_html, &error)) {
    return abort_staging(std::move(error));
  }
  std::uint64_t copied = html_bytes;

  if (tree) {
    const auto root = PathFromUtf8(target.assets_root);
    if (root.empty() || !std::filesystem::is_directory(root, code)) {
      return abort_staging("asset root does not exist: " +
                           target.assets_root);
    }
    std::filesystem::recursive_directory_iterator it(root, code);
    if (code) {
      return abort_staging("cannot read asset root " + target.assets_root +
                           ": " + code.message());
    }
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
      const auto& entry = *it;
      std::error_code link_error;
      const bool symlink = entry.is_symlink(link_error);
      if (link_error) {
        return abort_staging("cannot stat asset " + Utf8Path(entry.path()) +
                             ": " + link_error.message());
      }
      // A symlink can point outside the asset root, i.e. at a file the
      // resolver never authorized; fail instead of copying through it.
      if (symlink) {
        return abort_staging("asset is a symbolic link: " +
                             Utf8Path(entry.path()));
      }
      if (!PathEquals(entry.path(), source)) {
        const bool regular = entry.is_regular_file(code);
        if (code) {
          return abort_staging("cannot stat asset " + Utf8Path(entry.path()) +
                               ": " + code.message());
        }
        if (regular) {
          const auto relative =
              std::filesystem::relative(entry.path(), root, code);
          if (code) {
            return abort_staging("cannot resolve asset " +
                                 Utf8Path(entry.path()) + ": " +
                                 code.message());
          }
          const std::string relative_text = relative.generic_string();
          if (PassesFilters(relative_text, target.assets_include,
                            target.assets_exclude)) {
            // The target itself is already at staged_html; a tree resource
            // whose relative path names the same file must not replace it.
            const auto destination = stage_dir / relative;
            if (!PathEquals(destination, staged_html)) {
              const auto bytes = FileSize(entry.path());
              if (max_bytes > 0 &&
                  copied + bytes > static_cast<std::uint64_t>(max_bytes)) {
                return abort_staging(
                    "staged assets exceed assets.maxBytes at " +
                    Utf8Path(relative));
              }
              created.push_back(destination);
              std::string copy_error;
              if (!CopyFile(entry.path(), destination, &copy_error)) {
                return abort_staging(std::move(copy_error));
              }
              copied += bytes;
            }
          }
        }
      }
      it.increment(code);
      if (code) {
        // The error_code overload is the point: the throwing range-for used to
        // escape StageTargetHtml (and terminate the worker) on an unreadable
        // subdirectory. Fail the target with a message instead.
        return abort_staging("cannot read asset tree " + target.assets_root +
                             ": " + code.message());
      }
    }
  }

  outcome.html_path = staged_html;
  outcome.copied_bytes = copied;
  outcome.staged = true;
  return outcome;
}

} // namespace htmlppt
