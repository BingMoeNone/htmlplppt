#include "test_main.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/utf8_path.h"

using htmlppt::ConfigDirFor;
using htmlppt::PathEquals;
using htmlppt::PathFromUtf8;
using htmlppt::PathIsInside;
using htmlppt::ResolveConfigPath;
using htmlppt::Utf8Path;

namespace {

std::u8string Utf8Bytes() {
  std::u8string value;
  value.push_back(static_cast<char8_t>(0xE6));
  value.push_back(static_cast<char8_t>(0xBC));
  value.push_back(static_cast<char8_t>(0x94));
  value.push_back(static_cast<char8_t>(0xE7));
  value.push_back(static_cast<char8_t>(0xA4));
  value.push_back(static_cast<char8_t>(0xBA));
  return value;
}

} // namespace

HTMLPPT_TEST_CASE(utf8_path_preserves_utf8_bytes) {
  const std::filesystem::path path(Utf8Bytes());
  const std::string expected = "\xE6\xBC\x94\xE7\xA4\xBA";
  HTMLPPT_CHECK_EQ(Utf8Path(path), expected);
}

HTMLPPT_TEST_CASE(utf8_path_preserves_ascii) {
  const std::filesystem::path path("/tmp/page1.html");
  HTMLPPT_CHECK_EQ(Utf8Path(path), std::string{"/tmp/page1.html"});
}

HTMLPPT_TEST_CASE(utf8_path_round_trips_utf8_bytes) {
  const std::string bytes = "\xE6\xBC\x94\xE7\xA4\xBA";
  const std::filesystem::path path = PathFromUtf8(bytes);
  HTMLPPT_CHECK_EQ(Utf8Path(path), bytes);
}

HTMLPPT_TEST_CASE(utf8_path_builds_ascii_path) {
  const std::filesystem::path path = PathFromUtf8("/tmp/page1.html");
  HTMLPPT_CHECK_EQ(Utf8Path(path), std::string{"/tmp/page1.html"});
}

HTMLPPT_TEST_CASE(utf8_config_dir_resolves_a_bare_filename_against_the_cwd) {
  const auto cwd = std::filesystem::temp_directory_path() / "htmlppt-demo";
  HTMLPPT_CHECK_EQ(ConfigDirFor("watcher.json", cwd), cwd);
  HTMLPPT_CHECK_EQ(ConfigDirFor("sub/watcher.json", cwd), cwd / "sub");
  HTMLPPT_CHECK_EQ(ConfigDirFor(cwd / "watcher.json", cwd), cwd);
  HTMLPPT_CHECK(ConfigDirFor("", cwd).empty());
}

HTMLPPT_TEST_CASE(utf8_resolve_config_path_keeps_a_bare_name_absolute) {
  const auto cwd = std::filesystem::temp_directory_path() / "htmlppt-demo";
  HTMLPPT_CHECK_EQ(ResolveConfigPath("watcher.json", cwd), cwd / "watcher.json");
  HTMLPPT_CHECK_EQ(ResolveConfigPath(cwd / "watcher.json", cwd),
                   cwd / "watcher.json");
  // ConfigDirFor now shares the same resolution, so a bare name keeps the
  // legacy mirror enabled.
  HTMLPPT_CHECK_EQ(ConfigDirFor("watcher.json", cwd),
                   ResolveConfigPath("watcher.json", cwd).parent_path());
  HTMLPPT_CHECK(ResolveConfigPath("", cwd).empty());
}

HTMLPPT_TEST_CASE(utf8_path_equality_is_case_insensitive_on_windows_only) {
  const auto left = std::filesystem::path("C:/Demo/Audit.html");
  const auto right = std::filesystem::path("C:/Demo/audit.html");
#ifdef _WIN32
  HTMLPPT_CHECK(PathEquals(left, right));
#else
  HTMLPPT_CHECK(!PathEquals(left, right));
#endif
  HTMLPPT_CHECK(PathEquals(std::filesystem::path("C:/Demo/a/../b.html"),
                           std::filesystem::path("C:/Demo/b.html")));
  HTMLPPT_CHECK(!PathEquals(std::filesystem::path("C:/Demo/a.html"),
                            std::filesystem::path("C:/Demo/b.html")));
}

HTMLPPT_TEST_CASE(utf8_path_inside_detects_ancestors_not_siblings) {
  const auto root = std::filesystem::path("C:/work/demo");
  HTMLPPT_CHECK(PathIsInside(root / "state" / "crash" / "1.dmp", root));
  HTMLPPT_CHECK(!PathIsInside(root.parent_path() / "other" / "1.dmp", root));
  HTMLPPT_CHECK(!PathIsInside(root, root));
#ifdef _WIN32
  HTMLPPT_CHECK(PathIsInside(std::filesystem::path("C:/WORK/Demo/state/x"),
                             std::filesystem::path("c:/work/demo")));
#else
  HTMLPPT_CHECK(!PathIsInside(std::filesystem::path("/WORK/Demo/state/x"),
                              std::filesystem::path("/work/demo")));
#endif
}

HTMLPPT_TEST_CASE(utf8_write_file_atomically_replaces_the_target) {
  const auto dir =
      std::filesystem::temp_directory_path() / "htmlppt-atomic-test";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  const auto path = dir / "worker-plan.json";

  HTMLPPT_CHECK(htmlppt::WriteFileAtomically(path, "first\n", nullptr));
  HTMLPPT_CHECK(htmlppt::WriteFileAtomically(path, "second\n", nullptr));
  std::ifstream input(path, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  HTMLPPT_CHECK_EQ(text, std::string{"second\n"});
  // No temp file is left behind.
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "worker-plan.json.tmp"));

  // A missing directory fails and reports a message.
  const auto missing = dir / "missing" / "plan.json";
  std::string write_error;
  HTMLPPT_CHECK(!htmlppt::WriteFileAtomically(missing, "x", &write_error));
  HTMLPPT_CHECK(!write_error.empty());
  std::filesystem::remove_all(dir, error);
}
