#include "test_main.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

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
