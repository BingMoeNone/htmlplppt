#include "test_main.h"

#include <string>

#include "core/config.h"

using htmlppt::ConfigLoadResult;
using htmlppt::IssueSeverity;
using htmlppt::LoadConfigFromString;

namespace {

ConfigLoadResult Parse(std::string_view text) {
  return LoadConfigFromString(text, std::filesystem::path{"watcher.json"}, {});
}

bool HasError(const ConfigLoadResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == IssueSeverity::Error) {
      return true;
    }
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(config_parses_minimal) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [
      { "slide": 5, "html": "page1.html" }
    ]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(*result.config->active.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(result.config->active.targets[0].html, std::string{"page1.html"});
}

HTMLPPT_TEST_CASE(config_parses_nested_target_and_defaults) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": {
      "trigger": "arrive",
      "cover": { "fadeMs": 200, "holdMs": 500, "maxHoldMs": 8000 },
      "edge": { "maxLiveMs": 120000, "prewarmProfile": true },
      "scheduling": "aggressive",
      "maxConsecutiveTriggerFailures": 4
    },
    "targets": [{
      "match": { "slideTitle": "Audit", "mode": "exact" },
      "html": "audit.html",
      "edgeProfile": "tef-live-audit",
      "stageName": "audit.html",
      "cover": { "maxHoldMs": 12000 },
      "enabled": false,
      "tags": { "section": "audit" }
    }]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& config = *result.config;
  HTMLPPT_CHECK_EQ(config.defaults.trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(config.defaults.cover.fade_ms, 200);
  HTMLPPT_CHECK_EQ(config.defaults.cover.hold_ms, 500);
  HTMLPPT_CHECK_EQ(config.defaults.cover.max_hold_ms, 8000);
  HTMLPPT_CHECK_EQ(config.defaults.edge.max_live_ms, 120000);
  HTMLPPT_CHECK(config.defaults.edge.prewarm_profile);
  HTMLPPT_CHECK_EQ(config.defaults.scheduling, std::string{"aggressive"});
  HTMLPPT_CHECK_EQ(config.defaults.max_consecutive_trigger_failures, 4);
  const auto& target = config.active.targets[0];
  HTMLPPT_CHECK(target.match.has_value());
  HTMLPPT_CHECK_EQ(target.match->slide_title, std::string{"Audit"});
  HTMLPPT_CHECK_EQ(target.cover->max_hold_ms, 12000);
  HTMLPPT_CHECK(!target.enabled);
  HTMLPPT_CHECK_EQ(target.tags.at("section"), std::string{"audit"});
}

HTMLPPT_TEST_CASE(config_rejects_unknown_fields) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "a.html", "mystery": true }]
  })");
  HTMLPPT_CHECK(HasError(result));
  bool found = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "targets[0].mystery") {
      found = true;
    }
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(config_rejects_invalid_enum_and_missing_html) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "", "trigger": "teleport" }]
  })");
  HTMLPPT_CHECK(HasError(result));
  bool trigger_error = false;
  bool html_error = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "targets[0].trigger") trigger_error = true;
    if (issue.path == "targets[0].html") html_error = true;
  }
  HTMLPPT_CHECK(trigger_error);
  HTMLPPT_CHECK(html_error);
}

HTMLPPT_TEST_CASE(config_rejects_bad_json) {
  const auto result = Parse(R"({ "version": 1, )");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_cli_map_override_is_appended) {
  htmlppt::CliConfigOverrides overrides;
  overrides.map_overrides[7] = "override.html";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 5, "html": "page1.html" }]
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(*result.config->active.targets.back().slide, 7);
  HTMLPPT_CHECK_EQ(result.config->active.targets.back().html, std::string{"override.html"});
}

HTMLPPT_TEST_CASE(config_partial_edge_keeps_default_args) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": { "edge": { "args": ["--foo"] } },
    "targets": [{ "slide": 1, "html": "a.html", "edge": { "maxLiveMs": 1000 } }]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& target = result.config->active.targets[0];
  HTMLPPT_CHECK(target.edge.has_value());
  HTMLPPT_CHECK_EQ(target.edge->args.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(target.edge->args[0], std::string{"--foo"});
  HTMLPPT_CHECK_EQ(target.edge->max_live_ms, 1000);
}

HTMLPPT_TEST_CASE(config_cli_map_inherits_defaults) {
  htmlppt::CliConfigOverrides overrides;
  overrides.map_overrides[7] = "override.html";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": { "trigger": "arrive", "onFailure": "safe" },
    "targets": []
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& target = result.config->active.targets.back();
  HTMLPPT_CHECK_EQ(target.trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(target.on_failure, std::string{"safe"});
}

HTMLPPT_TEST_CASE(config_reads_large_max_bytes) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{
      "slide": 1,
      "html": "a.html",
      "assets": { "mode": "tree", "maxBytes": 4294967296 }
    }]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active.targets[0].assets.max_bytes,
                   std::int64_t{4294967296});
}
