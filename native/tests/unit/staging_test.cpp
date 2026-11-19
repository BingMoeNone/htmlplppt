#include "test_main.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "core/staging.h"

using htmlppt::ResolvedTarget;
using htmlppt::StageOutcome;
using htmlppt::StageTargetHtml;

namespace {

std::filesystem::path MakeTempDir(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / name;
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  return dir;
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

ResolvedTarget MakeTarget(const std::filesystem::path& html,
                          const std::string& mode,
                          const std::filesystem::path& root = {}) {
  ResolvedTarget target;
  target.slide = 1;
  target.html_path = html.string();
  target.stage_name = "page1.html";
  target.assets_mode = mode;
  target.assets_root = root.string();
  return target;
}

} // namespace

HTMLPPT_TEST_CASE(staging_direct_mode_keeps_the_source_path) {
  const auto dir = MakeTempDir("htmlppt-staging-direct");
  const auto html = dir / "page.html";
  WriteFile(html, "<html>direct</html>");
  const auto outcome =
      StageTargetHtml(MakeTarget(html, "direct"), dir / "stage", 0);
  HTMLPPT_CHECK(outcome.staged);
  HTMLPPT_CHECK_EQ(outcome.html_path, html);
  HTMLPPT_CHECK_EQ(outcome.copied_bytes, std::uint64_t{0});
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

HTMLPPT_TEST_CASE(staging_file_mode_copies_only_the_html) {
  const auto dir = MakeTempDir("htmlppt-staging-file");
  const auto html = dir / "source.html";
  WriteFile(html, "<html>file</html>");
  const auto outcome =
      StageTargetHtml(MakeTarget(html, "file"), dir / "stage", 0);
  HTMLPPT_CHECK(outcome.staged);
  HTMLPPT_CHECK_EQ(outcome.html_path, dir / "stage" / "page1.html");
  HTMLPPT_CHECK(std::filesystem::is_regular_file(outcome.html_path));
  // The asset root is not copied in file mode.
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "stage" / "source.html"));
  HTMLPPT_CHECK(outcome.copied_bytes > 0);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

HTMLPPT_TEST_CASE(staging_tree_mode_copies_the_asset_tree) {
  const auto dir = MakeTempDir("htmlppt-staging-tree");
  const auto root = dir / "assets";
  WriteFile(root / "page.html", "<html>tree</html>");
  WriteFile(root / "css" / "style.css", "body{}");
  WriteFile(root / "img" / "logo.svg", "<svg/>");
  const auto outcome =
      StageTargetHtml(MakeTarget(root / "page.html", "tree", root),
                      dir / "stage", 0);
  HTMLPPT_CHECK(outcome.staged);
  HTMLPPT_CHECK_EQ(outcome.html_path, dir / "stage" / "page1.html");
  HTMLPPT_CHECK(std::filesystem::is_regular_file(dir / "stage" / "css" /
                                                 "style.css"));
  HTMLPPT_CHECK(std::filesystem::is_regular_file(dir / "stage" / "img" /
                                                 "logo.svg"));
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

HTMLPPT_TEST_CASE(staging_missing_source_fails_with_a_message) {
  const auto dir = MakeTempDir("htmlppt-staging-missing");
  const auto outcome = StageTargetHtml(MakeTarget(dir / "nope.html", "file"),
                                       dir / "stage", 0);
  HTMLPPT_CHECK(!outcome.staged);
  HTMLPPT_CHECK(outcome.error.find("does not exist") != std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

HTMLPPT_TEST_CASE(staging_max_bytes_aborts_instead_of_truncating) {
  const auto dir = MakeTempDir("htmlppt-staging-maxbytes");
  const auto root = dir / "assets";
  WriteFile(root / "page.html", "0123456789");
  WriteFile(root / "big.bin", std::string(4096, 'x'));
  const auto outcome = StageTargetHtml(
      MakeTarget(root / "page.html", "tree", root), dir / "stage", 100);
  HTMLPPT_CHECK(!outcome.staged);
  HTMLPPT_CHECK(outcome.error.find("maxBytes") != std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

HTMLPPT_TEST_CASE(staging_tree_does_not_overwrite_the_staged_html) {
  const auto dir = MakeTempDir("htmlppt-staging-overwrite");
  const auto root = dir / "pages";
  // The target itself is pages/audit/index.html but lands as audit.html. A
  // sibling pages/audit.html must not replace that staged page with its own
  // bytes during the tree copy.
  WriteFile(root / "audit" / "index.html", "<html>target</html>");
  WriteFile(root / "audit.html", "<html>resource</html>");
  auto target = MakeTarget(root / "audit" / "index.html", "tree", root);
  target.stage_name = "audit.html";

  const auto outcome = StageTargetHtml(target, dir / "stage", 0);
  HTMLPPT_CHECK(outcome.staged);
  HTMLPPT_CHECK_EQ(outcome.html_path, dir / "stage" / "audit.html");
  const std::string staged = ReadFile(outcome.html_path);
  HTMLPPT_CHECK(staged.find("target") != std::string::npos);
  HTMLPPT_CHECK(staged.find("resource") == std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

// On Linux the iterator increment is the only way to reach an unreadable
// subdirectory after the range-for constructor succeeded. The old range-for
// threw filesystem_error out of StageTargetHtml; the error_code increment must
// turn it into a staged=false outcome instead.
HTMLPPT_TEST_CASE(staging_tree_unreadable_subdirectory_fails_cleanly) {
#ifndef _WIN32
  const auto dir = MakeTempDir("htmlppt-staging-unreadable");
  const auto root = dir / "assets";
  WriteFile(root / "page.html", "<html>tree</html>");
  WriteFile(root / "blocked" / "asset.txt", "asset");
  HTMLPPT_CHECK_EQ(::chmod((root / "blocked").c_str(), 0), 0);

  const auto outcome =
      StageTargetHtml(MakeTarget(root / "page.html", "tree", root),
                      dir / "stage", 0);
  HTMLPPT_CHECK_EQ(::chmod((root / "blocked").c_str(), 0700), 0);

  HTMLPPT_CHECK(!outcome.staged);
  HTMLPPT_CHECK(outcome.error.find("cannot read asset tree") !=
                std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
#endif
}

HTMLPPT_TEST_CASE(staging_tree_mode_applies_include_and_exclude_globs) {
  const auto dir = MakeTempDir("htmlppt-staging-globs");
  const auto root = dir / "assets";
  WriteFile(root / "page.html", "<html></html>");
  WriteFile(root / "css" / "style.css", "body{}");
  WriteFile(root / "js" / "app.js", "console.log(1)");
  WriteFile(root / "img" / "logo.png", "png");

  auto target = MakeTarget(root / "page.html", "tree", root);
  target.assets_include = {"**/*.css", "**/*.png"};
  target.assets_exclude = {"img/**"};
  const auto outcome = StageTargetHtml(target, dir / "stage", 0);

  HTMLPPT_CHECK(outcome.staged);
  HTMLPPT_CHECK(std::filesystem::is_regular_file(dir / "stage" / "css" /
                                                 "style.css"));
  // Not whitelisted.
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "stage" / "js" / "app.js"));
  // Exclude wins over include.
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "stage" / "img" / "logo.png"));
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}