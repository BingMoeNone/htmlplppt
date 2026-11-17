#include "test_main.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/edge_spec.h"
#include "core/utf8_path.h"

using htmlppt::BuildEdgeUrl;
using htmlppt::DefaultEdgeFlags;
using htmlppt::EdgeConfig;
using htmlppt::EdgeLaunchSpec;
using htmlppt::MakeEdgeLaunchSpec;
using htmlppt::PathFromUtf8;
using htmlppt::ResolvedTarget;
using htmlppt::Utf8Path;

namespace {

bool Contains(const std::vector<std::string>& values,
              const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

HTMLPPT_TEST_CASE(edge_url_encodes_unicode_and_spaces) {
  ResolvedTarget target;
  std::string path = "/tmp/";
  path += "\xE6\xBC\x94";  // UTF-8 for a CJK character.
  path += "\xE7\xA4\xBA";
  path += " page.html";
  target.html_path = path;
  target.query = "lang=zh";
  target.fragment = "step2";
  const std::string url = BuildEdgeUrl(target);
  HTMLPPT_CHECK_EQ(url.rfind("file:///", 0), std::string::size_type{0});
  HTMLPPT_CHECK(url.find("%20") != std::string::npos);
  HTMLPPT_CHECK(url.find("%E6%BC%94") != std::string::npos);
  HTMLPPT_CHECK(url.find("?lang=zh") != std::string::npos);
  HTMLPPT_CHECK(url.find("#step2") != std::string::npos);
}

HTMLPPT_TEST_CASE(edge_url_keeps_query_and_fragment_empty) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  const std::string url = BuildEdgeUrl(target);
  HTMLPPT_CHECK_EQ(url, std::string{"file:///tmp/page1.html"});
}

HTMLPPT_TEST_CASE(edge_url_handles_windows_drive_path) {
  ResolvedTarget target;
  target.html_path = "C:\\Users\\A B\\page.html";
  HTMLPPT_CHECK_EQ(BuildEdgeUrl(target),
                   std::string{"file:///C:/Users/A%20B/page.html"});
}

HTMLPPT_TEST_CASE(edge_url_handles_unc_path) {
  ResolvedTarget target;
  target.html_path = "\\\\server\\share\\page.html";
  // Edge/Chromium normalizes the extra leading slash for UNC paths on
  // Windows; this test pins the current core representation.
  HTMLPPT_CHECK_EQ(BuildEdgeUrl(target),
                   std::string{"file:////server/share/page.html"});
}

HTMLPPT_TEST_CASE(edge_url_percent_encodes_path_reserved_characters) {
  ResolvedTarget target;
  target.html_path = "/tmp/a?b#c%.html";
  const std::string url = BuildEdgeUrl(target);
  HTMLPPT_CHECK_EQ(url, std::string{"file:///tmp/a%3Fb%23c%25.html"});
  HTMLPPT_CHECK(url.find("%3F") != std::string::npos);
  HTMLPPT_CHECK(url.find("%23") != std::string::npos);
  HTMLPPT_CHECK(url.find("%25") != std::string::npos);
}

HTMLPPT_TEST_CASE(edge_spec_uses_profile_and_default_flags) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  target.edge_profile = "htmlppt-deck-t05";
  const std::filesystem::path temp_dir = "/tmp";
  const EdgeLaunchSpec spec = MakeEdgeLaunchSpec(
      "/usr/bin/msedge", temp_dir, target, EdgeConfig{});
  const std::filesystem::path expected_profile =
      temp_dir / "htmlppt-deck-t05";
  HTMLPPT_CHECK_EQ(spec.executable, std::filesystem::path("/usr/bin/msedge"));
  HTMLPPT_CHECK_EQ(spec.profile_dir, expected_profile);
  HTMLPPT_CHECK_EQ(spec.url, BuildEdgeUrl(target));
  HTMLPPT_CHECK(Contains(spec.arguments,
                         "--app=" + BuildEdgeUrl(target)));
  HTMLPPT_CHECK(Contains(spec.arguments,
                         "--user-data-dir=" + Utf8Path(expected_profile)));
  HTMLPPT_CHECK(Contains(spec.arguments, "--start-fullscreen"));
  HTMLPPT_CHECK(Contains(spec.arguments, "--no-first-run"));
  HTMLPPT_CHECK(Contains(spec.arguments, "--disable-sync"));
  HTMLPPT_CHECK(Contains(spec.arguments, "--disable-breakpad"));
  HTMLPPT_CHECK(!DefaultEdgeFlags().empty());
}

HTMLPPT_TEST_CASE(edge_spec_uses_utf8_profile_directory) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  target.edge_profile = "profile-\xE6\xBC\x94";
  const std::filesystem::path temp_dir = "/tmp";
  const EdgeLaunchSpec spec = MakeEdgeLaunchSpec(
      "/usr/bin/msedge", temp_dir, target, EdgeConfig{});
  const std::filesystem::path expected_profile =
      temp_dir / PathFromUtf8("profile-\xE6\xBC\x94");
  HTMLPPT_CHECK_EQ(spec.profile_dir, expected_profile);
  HTMLPPT_CHECK_EQ(Utf8Path(spec.profile_dir), Utf8Path(expected_profile));
  HTMLPPT_CHECK(Contains(spec.arguments,
                         "--user-data-dir=" + Utf8Path(expected_profile)));
}

HTMLPPT_TEST_CASE(edge_spec_uses_configured_flags_when_present) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  target.edge_profile = "profile";
  EdgeConfig config;
  config.args = {"--custom-flag"};
  const EdgeLaunchSpec spec =
      MakeEdgeLaunchSpec("/usr/bin/msedge", "/tmp", target, config);
  bool found = false;
  for (const auto& arg : spec.arguments) {
    if (arg == "--custom-flag") found = true;
  }
  HTMLPPT_CHECK(found);
}
