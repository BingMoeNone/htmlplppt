#include "test_main.h"
#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/config.h"
#include "core/deck_info.h"
#include "core/resolve.h"

using htmlppt::Config;
using htmlppt::IssueSeverity;
using htmlppt::ResolveResult;
using htmlppt::ResolveState;

namespace {

struct Fixture {
  htmlppt::test::TempDir dir;

  Fixture() {
    Write("deck.pptx", "PK");
    Write("page1.html", "<html>1</html>");
    Write("page2.html", "<html>2</html>");
  }

  void Write(const std::filesystem::path& relative,
             const std::string& content) const {
    const auto path = dir.path() / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
  }

  std::filesystem::path ConfigPath() const { return dir.path() / "watcher.json"; }
};

Config ParseConfig(const Fixture& fixture, std::string_view text) {
  const auto result = htmlppt::LoadConfigFromString(text, fixture.ConfigPath(), {});
  HTMLPPT_CHECK(result.config.has_value());
  return *result.config;
}

htmlppt::DeckInfo MakeDeck() {
  htmlppt::DeckInfo deck;
  deck.path = "deck.pptx";
  deck.slides = {
      {1, "First", "Slide1"},
      {2, "Second", "Slide2"},
      {3, "", "NamedSlide"},
  };
  return deck;
}

htmlppt::ResolveOptions DefaultOptions() {
  htmlppt::ResolveOptions options;
  options.deck_fingerprint = "0123456789abcdef";
  return options;
}

bool HasError(const ResolveResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

bool HasErrorAt(const ResolveResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path && issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

bool HasWarningAt(const ResolveResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path && issue.severity == IssueSeverity::Warning) return true;
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(resolve_slide_target_with_deck_info) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 2, "html": "page1.html", "label": "Second" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
  HTMLPPT_CHECK_EQ(result.targets[0].label, std::string{"Second"});
  HTMLPPT_CHECK_EQ(result.targets[0].stage_name,
                   std::string{"target-01-page1.html"});
  HTMLPPT_CHECK_EQ(result.targets[0].edge_profile,
                   std::string{"htmlppt-0123456789abcdef-t01"});
  HTMLPPT_CHECK_EQ(result.targets[0].html_path,
                   std::filesystem::weakly_canonical(
                       fixture.dir.path() / "page1.html").string());
  HTMLPPT_CHECK(result.targets[0].state == ResolveState::Resolved);
}

HTMLPPT_TEST_CASE(resolve_slide_out_of_range) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 9, "html": "page1.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].slide"));
}

HTMLPPT_TEST_CASE(resolve_slide_without_deck_info_warns) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 2, "html": "page1.html" }]
  })");
  const auto result = htmlppt::ResolveTargets(config, nullptr, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK(HasWarningAt(result, "targets[0].slide"));
}

HTMLPPT_TEST_CASE(resolve_title_exact) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Second", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
  HTMLPPT_CHECK_EQ(result.targets[0].label, std::string{"Slide 2"});
}

HTMLPPT_TEST_CASE(resolve_title_prefix) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Sec", "mode": "prefix" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
}

HTMLPPT_TEST_CASE(resolve_title_regex) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "^Sec.nd$", "mode": "regex" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
}

HTMLPPT_TEST_CASE(resolve_title_requires_deck_info) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Second", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto result = htmlppt::ResolveTargets(config, nullptr, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match"));
}

HTMLPPT_TEST_CASE(resolve_title_no_match) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Missing", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match.slideTitle"));
}

HTMLPPT_TEST_CASE(resolve_title_multiple_matches) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": ".*", "mode": "regex" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match.slideTitle"));
}

HTMLPPT_TEST_CASE(resolve_slide_name_exact) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideName": "NamedSlide", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 3);
}

HTMLPPT_TEST_CASE(resolve_invalid_regex) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "[", "mode": "regex" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match.mode"));
}

HTMLPPT_TEST_CASE(resolve_missing_html) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "missing.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].html"));
}

HTMLPPT_TEST_CASE(resolve_outside_folder_rejected) {
  Fixture fixture;
  fixture.Write("config/deck.pptx", "PK");
  fixture.Write("outside.html", "<html>outside</html>");
  const auto parsed = htmlppt::LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "../outside.html" }]
  })", fixture.dir.path() / "config" / "watcher.json", {});
  HTMLPPT_CHECK(parsed.config.has_value());
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(*parsed.config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].html"));
}

HTMLPPT_TEST_CASE(resolve_outside_folder_allowed) {
  Fixture fixture;
  fixture.Write("config/deck.pptx", "PK");
  fixture.Write("outside.html", "<html>outside</html>");
  const auto parsed = htmlppt::LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "../outside.html" }]
  })", fixture.dir.path() / "config" / "watcher.json", {});
  HTMLPPT_CHECK(parsed.config.has_value());
  auto options = DefaultOptions();
  options.allow_outside_folder = true;
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(*parsed.config, &deck, options);
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
}

HTMLPPT_TEST_CASE(resolve_absolute_html_inside_folder) {
  Fixture fixture;
  const std::string html =
      (fixture.dir.path() / "page1.html").generic_string();
  const std::string text =
      R"({"version":1,"deck":"deck.pptx","targets":[{"slide":1,"html":")" +
      html + R"("}]})";
  const Config config = ParseConfig(fixture, text);
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
}

HTMLPPT_TEST_CASE(resolve_html_query_merges) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      {
        "slide": 1,
        "html": "page1.html?a=1",
        "htmlQuery": "?b=2#frag"
      },
      {
        "slide": 2,
        "html": "page2.html",
        "htmlQuery": "c=3"
      }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(result.targets[0].query, std::string{"a=1&b=2"});
  HTMLPPT_CHECK_EQ(result.targets[0].fragment, std::string{"frag"});
  HTMLPPT_CHECK_EQ(result.targets[1].query, std::string{"c=3"});
  HTMLPPT_CHECK_EQ(result.targets[1].fragment, std::string{""});
}

HTMLPPT_TEST_CASE(resolve_html_query_duplicate_keys_allowed) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      {
        "slide": 1,
        "html": "page1.html?a=1&a=2",
        "htmlQuery": "?b=1&b=2"
      },
      {
        "slide": 2,
        "html": "page2.html",
        "htmlQuery": "?c=1&c=2"
      }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(result.targets[0].query, std::string{"a=1&a=2&b=1&b=2"});
  HTMLPPT_CHECK_EQ(result.targets[1].query, std::string{"c=1&c=2"});
}

HTMLPPT_TEST_CASE(resolve_html_query_conflict) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html?a=1",
      "htmlQuery": "?a=2"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].htmlQuery"));
}

HTMLPPT_TEST_CASE(resolve_fragment_conflict) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html#one",
      "htmlQuery": "#two"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].htmlQuery"));
}

HTMLPPT_TEST_CASE(resolve_explicit_stage_name) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html",
      "stageName": "custom.html",
      "edgeProfile": "custom-profile"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].stage_name, std::string{"custom.html"});
  HTMLPPT_CHECK_EQ(result.targets[0].edge_profile,
                   std::string{"custom-profile"});
}

HTMLPPT_TEST_CASE(resolve_rejects_stage_name_with_separator) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html",
      "stageName": "a/b.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].stageName"));
}

HTMLPPT_TEST_CASE(resolve_duplicate_slide) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html", "stageName": "one.html",
        "edgeProfile": "profile-one" },
      { "slide": 1, "html": "page2.html", "stageName": "two.html",
        "edgeProfile": "profile-two" }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[1].slide"));
}

HTMLPPT_TEST_CASE(resolve_duplicate_stage_name) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html", "stageName": "same.html",
        "edgeProfile": "profile-one" },
      { "slide": 2, "html": "page2.html", "stageName": "same.html",
        "edgeProfile": "profile-two" }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[1].stageName"));
}

HTMLPPT_TEST_CASE(resolve_duplicate_edge_profile) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html", "stageName": "one.html",
        "edgeProfile": "dup" },
      { "slide": 2, "html": "page2.html", "stageName": "two.html",
        "edgeProfile": "dup" }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[1].edgeProfile"));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.targets[0].edge_profile, std::string{"dup"});
}

HTMLPPT_TEST_CASE(resolve_disabled_target_is_listed) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html" },
      { "slide": 2, "html": "page2.html", "enabled": false }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{2});
  HTMLPPT_CHECK(result.targets[1].state == ResolveState::Disabled);
  HTMLPPT_CHECK_EQ(result.targets[1].slide, 0);
  HTMLPPT_CHECK(result.targets[1].html_path.empty());
}

HTMLPPT_TEST_CASE(resolve_missing_assets_root) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html",
      "assets": { "mode": "tree", "root": "assets" }
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].assets.root"));
}

HTMLPPT_TEST_CASE(resolve_zero_targets_warns) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": []
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK(HasWarningAt(result, "targets"));
}

HTMLPPT_TEST_CASE(resolve_missing_deck_errors) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "missing.pptx",
    "targets": [{ "slide": 1, "html": "page1.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "deck"));
}

HTMLPPT_TEST_CASE(resolve_table_contains_rows) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "page1.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  const std::string table = htmlppt::ResolveTableToText(result);
  HTMLPPT_CHECK(table.find("SLIDE") != std::string::npos);
  HTMLPPT_CHECK(table.find("page1.html") != std::string::npos);
  HTMLPPT_CHECK(table.find("resolved") != std::string::npos);
}
