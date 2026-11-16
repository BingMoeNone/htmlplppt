#include "test_main.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/edge_spec.h"

using htmlppt::BuildEdgeUrl;
using htmlppt::DefaultEdgeFlags;
using htmlppt::EdgeConfig;
using htmlppt::EdgeLaunchSpec;
using htmlppt::MakeEdgeLaunchSpec;
using htmlppt::ResolvedTarget;

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

HTMLPPT_TEST_CASE(edge_spec_uses_profile_and_default_flags) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  target.edge_profile = "htmlppt-deck-t05";
  const EdgeLaunchSpec spec = MakeEdgeLaunchSpec(
      "/usr/bin/msedge", "/tmp", target, EdgeConfig{});
  HTMLPPT_CHECK_EQ(spec.executable.string(), std::string{"/usr/bin/msedge"});
  HTMLPPT_CHECK_EQ(spec.profile_dir.string(),
                   std::string{"/tmp/htmlppt-deck-t05"});
  bool has_app = false;
  bool has_profile = false;
  bool has_fullscreen = false;
  for (const auto& arg : spec.arguments) {
    if (arg.rfind("--app=", 0) == 0) has_app = true;
    if (arg.rfind("--user-data-dir=", 0) == 0) has_profile = true;
    if (arg == "--start-fullscreen") has_fullscreen = true;
  }
  HTMLPPT_CHECK(has_app);
  HTMLPPT_CHECK(has_profile);
  HTMLPPT_CHECK(has_fullscreen);
  HTMLPPT_CHECK(!DefaultEdgeFlags().empty());
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
