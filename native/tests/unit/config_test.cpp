#include "test_main.h"

#include <string>

#include "core/config.h"
#include "core/config_issue.h"

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

bool HasIssueAt(const ConfigLoadResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path) return true;
  }
  return false;
}

bool HasIssueAtWithMessage(const ConfigLoadResult& result, std::string_view path,
                           std::string_view message) {
  for (const auto& issue : result.issues) {
    if (issue.path == path && issue.message == message) return true;
  }
  return false;
}

std::size_t CountIssuesAt(const ConfigLoadResult& result, std::string_view path) {
  std::size_t count = 0;
  for (const auto& issue : result.issues) {
    if (issue.path == path) ++count;
  }
  return count;
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

HTMLPPT_TEST_CASE(config_migrates_legacy_arrays) {
  const auto result = Parse(R"({
    "version": 1,
    "legacy": {
      "deckName": "deck-live.pptx",
      "targetSlides": [5, 6],
      "srcNames": ["page1.html", "page2.html"],
      "targetHtmls": ["page1.html", "page2.html"],
      "targetProfiles": ["tef-live-page1", "tef-live-page2"],
      "targetLabels": ["Page1", "Page2"]
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& config = *result.config;
  HTMLPPT_CHECK_EQ(config.active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(config.active.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(*config.active.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(config.active.targets[0].html, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].stage_name, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].edge_profile, std::string{"tef-live-page1"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].label, std::string{"Page1"});
  HTMLPPT_CHECK_EQ(config.active.targets[0].source_priority, 0);
}

HTMLPPT_TEST_CASE(config_rejects_unequal_legacy_arrays) {
  const auto result = Parse(R"({
    "version": 1,
    "legacy": {
      "deckName": "deck-live.pptx",
      "targetSlides": [5, 6],
      "srcNames": ["page1.html"],
      "targetHtmls": ["page1.html", "page2.html"],
      "targetProfiles": ["tef-live-page1", "tef-live-page2"],
      "targetLabels": ["Page1", "Page2"]
    }
  })");
  HTMLPPT_CHECK(HasError(result));
}

HTMLPPT_TEST_CASE(config_selects_single_profile_automatically) {
  const auto result = Parse(R"({
    "version": 1,
    "profiles": {
      "competition": {
        "deck": "deck-live.pptx",
        "targets": [{ "slide": 5, "html": "page1.html" }]
      }
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active_profile, std::string{"competition"});
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{1});
}

HTMLPPT_TEST_CASE(config_requires_profile_when_multiple_exist) {
  const auto result = Parse(R"({
    "version": 1,
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })");
  HTMLPPT_CHECK(HasError(result));
  bool found = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "activeProfile") found = true;
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(config_cli_profile_overrides_active_profile) {
  htmlppt::CliConfigOverrides overrides;
  overrides.profile = "b";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "activeProfile": "a",
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active_profile, std::string{"b"});
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"b.pptx"});
}

HTMLPPT_TEST_CASE(config_precedence_metadata_is_recorded) {
  htmlppt::CliConfigOverrides overrides;
  overrides.map_overrides[5] = "cli.html";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "activeProfile": "a",
    "targets": [{ "slide": 5, "html": "top.html" }],
    "profiles": {
      "a": {
        "deck": "a.pptx",
        "targets": [{ "slide": 5, "html": "profile.html" }]
      }
    },
    "legacy": {
      "deckName": "a.pptx",
      "targetSlides": [5],
      "srcNames": ["legacy.html"],
      "targetHtmls": ["legacy.html"],
      "targetProfiles": ["tef-live-legacy"],
      "targetLabels": ["Legacy"]
    }
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& targets = result.config->active.targets;
  HTMLPPT_CHECK_EQ(targets.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(targets[0].source_priority, 2);  // profile
  HTMLPPT_CHECK_EQ(targets[1].source_priority, 1);  // top-level
  HTMLPPT_CHECK_EQ(targets[2].source_priority, 0);  // legacy
  HTMLPPT_CHECK_EQ(targets[3].source_priority, 3);  // CLI map
}

HTMLPPT_TEST_CASE(config_print_round_trips) {
  const auto first = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": { "trigger": "arrive", "cover": { "maxHoldMs": 9000 } },
    "targets": [{
      "slide": 5,
      "html": "page1.html",
      "label": "Page1",
      "edgeProfile": "tef-live-page1",
      "stageName": "page1.html",
      "cover": { "holdMs": 600 }
    }]
  })");
  HTMLPPT_CHECK(first.config.has_value());
  HTMLPPT_CHECK(!HasError(first));
  const std::string serialized = htmlppt::ConfigToJson(*first.config);
  const auto second = LoadConfigFromString(
      serialized, std::filesystem::path{"watcher.json"}, {});
  HTMLPPT_CHECK(second.config.has_value());
  HTMLPPT_CHECK(!HasError(second));
  HTMLPPT_CHECK_EQ(second.config->active.deck, std::string{"deck-live.pptx"});
  HTMLPPT_CHECK_EQ(second.config->defaults.trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(second.config->defaults.cover.max_hold_ms, 9000);
  HTMLPPT_CHECK_EQ(second.config->active.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(second.config->active.targets[0].cover->hold_ms, 600);
  HTMLPPT_CHECK_EQ(second.config->active.targets[0].edge_profile,
                   std::string{"tef-live-page1"});
}

HTMLPPT_TEST_CASE(config_legacy_inherits_defaults) {
  const auto result = Parse(R"({
    "version": 1,
    "defaults": { "trigger": "arrive", "onFailure": "strict" },
    "legacy": {
      "deckName": "deck-live.pptx",
      "targetSlides": [5],
      "srcNames": ["page1.html"],
      "targetHtmls": ["page1.html"],
      "targetProfiles": ["tef-live-page1"],
      "targetLabels": ["Page1"]
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& target = result.config->active.targets[0];
  HTMLPPT_CHECK_EQ(target.trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(target.on_failure, std::string{"strict"});
}

HTMLPPT_TEST_CASE(config_profile_stage_inherits_top_level_root) {
  const auto result = Parse(R"({
    "version": 1,
    "activeProfile": "p",
    "stage": { "root": "top-root" },
    "profiles": {
      "p": { "stage": { "mode": "direct" }, "targets": [] }
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active.stage.mode, std::string{"direct"});
  HTMLPPT_CHECK_EQ(result.config->active.stage.root, std::string{"top-root"});
}

HTMLPPT_TEST_CASE(config_profile_stage_inherits_top_level_mode) {
  const auto result = Parse(R"({
    "version": 1,
    "activeProfile": "p",
    "stage": { "mode": "direct" },
    "profiles": {
      "p": { "stage": { "root": "profile-root" }, "targets": [] }
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active.stage.mode, std::string{"direct"});
  HTMLPPT_CHECK_EQ(result.config->active.stage.root, std::string{"profile-root"});
}

HTMLPPT_TEST_CASE(config_profile_omitting_stage_inherits_top_level) {
  const auto result = Parse(R"({
    "version": 1,
    "activeProfile": "p",
    "stage": { "mode": "direct" },
    "profiles": {
      "p": { "targets": [] }
    }
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.config->active.stage.mode, std::string{"direct"});
  HTMLPPT_CHECK_EQ(result.config->active.stage.root, std::string{""});
}

HTMLPPT_TEST_CASE(config_print_config_snapshot_reloads_without_issues) {
  const auto first = Parse(R"({
    "version": 1,
    "activeProfile": "a",
    "deck": "top.pptx",
    "stage": { "root": "top-root" },
    "defaults": { "trigger": "arrive" },
    "targets": [{ "slide": 5, "html": "top.html" }],
    "profiles": {
      "a": {
        "deck": "profile.pptx",
        "stage": { "mode": "direct" },
        "targets": [
          { "slide": 5, "html": "profile.html" },
          { "slide": 6, "html": "profile6.html" }
        ]
      }
    },
    "legacy": {
      "deckName": "legacy.pptx",
      "targetSlides": [5],
      "srcNames": ["legacy.html"],
      "targetHtmls": ["legacy.html"],
      "targetProfiles": ["tef-live-legacy"],
      "targetLabels": ["Legacy"]
    }
  })");
  HTMLPPT_CHECK(first.config.has_value());
  HTMLPPT_CHECK(!HasError(first));
  const std::string snapshot = htmlppt::ConfigToJson(*first.config);
  const auto second = LoadConfigFromString(
      snapshot, std::filesystem::path{"watcher.json"}, {});
  HTMLPPT_CHECK(second.config.has_value());
  HTMLPPT_CHECK(second.issues.empty());
  const auto& targets = second.config->active.targets;
  HTMLPPT_CHECK_EQ(targets.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(*targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(targets[0].html, std::string{"profile.html"});
  HTMLPPT_CHECK_EQ(*targets[1].slide, 6);
  HTMLPPT_CHECK_EQ(targets[1].html, std::string{"profile6.html"});
  HTMLPPT_CHECK_EQ(*targets[2].slide, 5);
  HTMLPPT_CHECK_EQ(targets[2].html, std::string{"top.html"});
  HTMLPPT_CHECK_EQ(*targets[3].slide, 5);
  HTMLPPT_CHECK_EQ(targets[3].html, std::string{"legacy.html"});
  HTMLPPT_CHECK_EQ(targets[0].trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(targets[1].trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(targets[2].trigger, std::string{"arrive"});
  HTMLPPT_CHECK_EQ(targets[3].trigger, std::string{"arrive"});
}

HTMLPPT_TEST_CASE(config_rejects_non_ascii_legacy_stage_name) {
  const std::string text = std::string(R"({
    "version": 1,
    "legacy": {
      "deckName": "deck-live.pptx",
      "targetSlides": [5],
      "srcNames": ["page1.html"],
      "targetHtmls": [")") + '\\' + R"(u5ba1.html"],
      "targetProfiles": ["tef-live-page1"],
      "targetLabels": ["Page1"]
    }
  })";
  const auto result = Parse(text);
  HTMLPPT_CHECK(HasError(result));
  bool found = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "legacy.targetHtmls[0]") found = true;
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(config_rejects_unknown_profile) {
  const auto result = Parse(R"({
    "version": 1,
    "activeProfile": "missing",
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })");
  HTMLPPT_CHECK(HasError(result));
  bool found = false;
  for (const auto& issue : result.issues) {
    if (issue.path == "activeProfile") found = true;
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(config_accepts_numeric_monitor) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{
      "slide": 5,
      "html": "page1.html",
      "display": { "monitor": 2 }
    }]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  const auto& target = result.config->active.targets[0];
  HTMLPPT_CHECK(target.display.has_value());
  HTMLPPT_CHECK_EQ(target.display->monitor, std::string{"2"});
}

HTMLPPT_TEST_CASE(config_rejects_partial_numeric_monitor) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{
      "slide": 5,
      "html": "page1.html",
      "display": { "monitor": "2abc" }
    }]
  })");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(HasIssueAt(result, "targets[0].display.monitor"));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_rejects_out_of_range_integers) {
  const auto result = Parse(R"({
    "version": 4294967297,
    "deck": "deck-live.pptx",
    "targets": [{
      "slide": 4294967297,
      "html": "a.html",
      "assets": { "maxBytes": 9223372036854775808 }
    }]
  })");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(!result.config.has_value());
  HTMLPPT_CHECK(HasIssueAtWithMessage(result, "version", "integer is out of 32-bit range"));
  HTMLPPT_CHECK(HasIssueAtWithMessage(result, "targets[0].slide",
                                      "integer is out of 32-bit range"));
  HTMLPPT_CHECK(HasIssueAtWithMessage(result, "targets[0].assets.maxBytes",
                                      "integer is out of int64 range"));
  HTMLPPT_CHECK(!HasIssueAtWithMessage(result, "version", "required integer is missing"));
}

HTMLPPT_TEST_CASE(config_rejects_zero_slide) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 0, "html": "a.html" }]
  })");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(HasIssueAtWithMessage(result, "targets[0].slide", "must be >= 1"));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_error_result_has_no_config) {
  const auto result = Parse(R"({
    "version": 1,
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_unknown_profile_has_no_config) {
  const auto result = Parse(R"({
    "version": 1,
    "activeProfile": "missing",
    "profiles": {
      "a": { "deck": "a.pptx", "targets": [] },
      "b": { "deck": "b.pptx", "targets": [] }
    }
  })");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_single_profile_ignores_unknown_cli_profile) {
  htmlppt::CliConfigOverrides overrides;
  overrides.profile = "other";
  const auto result = LoadConfigFromString(R"({
    "version": 1,
    "profiles": {
      "only": {
        "deck": "only.pptx",
        "targets": [{ "slide": 5, "html": "only.html" }]
      }
    }
  })", std::filesystem::path{"watcher.json"}, overrides);
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK_EQ(result.config->active_profile, std::string{"only"});
  HTMLPPT_CHECK_EQ(result.config->active.deck, std::string{"only.pptx"});
  HTMLPPT_CHECK_EQ(result.config->active.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.config->active.targets[0].html, std::string{"only.html"});
}

HTMLPPT_TEST_CASE(config_rejects_empty_on_failure) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "a.html", "onFailure": "" }]
  })");
  HTMLPPT_CHECK(HasError(result));
  HTMLPPT_CHECK(HasIssueAt(result, "targets[0].onFailure"));
  HTMLPPT_CHECK(!result.config.has_value());
}

HTMLPPT_TEST_CASE(config_reports_single_html_error) {
  const auto missing = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1 }]
  })");
  HTMLPPT_CHECK(HasError(missing));
  HTMLPPT_CHECK_EQ(CountIssuesAt(missing, "targets[0].html"), std::size_t{1});
  HTMLPPT_CHECK(HasIssueAtWithMessage(missing, "targets[0].html",
                                      "required string is missing"));

  const auto empty = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "" }]
  })");
  HTMLPPT_CHECK(HasError(empty));
  HTMLPPT_CHECK_EQ(CountIssuesAt(empty, "targets[0].html"), std::size_t{1});
  HTMLPPT_CHECK(HasIssueAtWithMessage(empty, "targets[0].html", "must not be empty"));
}

HTMLPPT_TEST_CASE(config_rejects_an_unknown_focus_strategy) {
  const auto bad = htmlppt::LoadConfigFromString(
      R"({"version":1,"deck":"d.pptx","targets":[{"slide":1,"html":"a.html","focus":{"strategy":"bogus"}}]})",
      "watcher.json", {});
  HTMLPPT_CHECK(htmlppt::HasErrorIssue(bad.issues));

  const auto good = htmlppt::LoadConfigFromString(
      R"({"version":1,"deck":"d.pptx","targets":[{"slide":1,"html":"a.html","focus":{"strategy":"attach-alt"}}]})",
      "watcher.json", {});
  HTMLPPT_CHECK(!htmlppt::HasErrorIssue(good.issues));
  HTMLPPT_CHECK(good.config.has_value());
  HTMLPPT_CHECK_EQ(good.config->active.targets[0].focus->strategy,
                   std::string{"attach-alt"});
}

HTMLPPT_TEST_CASE(config_round_trips_the_focus_timeout) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": {
      "focus": { "timeoutMs": 4000 }
    },
    "targets": [
      {
        "slide": 1,
        "html": "a.html",
        "focus": { "strategy": "attach-alt", "require": false,
                   "timeoutMs": 2500 }
      },
      { "slide": 2, "html": "b.html" }
    ]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK(result.config->defaults.focus.timeout_ms.has_value());
  HTMLPPT_CHECK_EQ(*result.config->defaults.focus.timeout_ms, 4000);
  HTMLPPT_CHECK(result.config->active.targets[0].focus.has_value());
  HTMLPPT_CHECK(result.config->active.targets[0].focus->timeout_ms.has_value());
  HTMLPPT_CHECK_EQ(*result.config->active.targets[0].focus->timeout_ms, 2500);
  HTMLPPT_CHECK(!result.config->active.targets[1].focus.has_value());

  // The editor's save path re-serializes the effective config; a field it drops
  // would be lost the next time the operator saves.
  const std::string json = htmlppt::ConfigToJson(*result.config);
  HTMLPPT_CHECK(json.find("\"timeoutMs\": 4000") != std::string::npos);
  HTMLPPT_CHECK(json.find("\"timeoutMs\": 2500") != std::string::npos);

  const auto reloaded = Parse(json);
  HTMLPPT_CHECK(reloaded.config.has_value());
  HTMLPPT_CHECK(!HasError(reloaded));
  HTMLPPT_CHECK_EQ(*reloaded.config->defaults.focus.timeout_ms, 4000);
  HTMLPPT_CHECK_EQ(*reloaded.config->active.targets[0].focus->timeout_ms, 2500);
}

// A target focus block with no timeout keeps the defaults timeout, exactly like
// strategy/require inherit from the base focus config.
HTMLPPT_TEST_CASE(config_focus_timeout_inherits_from_defaults) {
  const auto result = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": { "focus": { "timeoutMs": 3500 } },
    "targets": [
      { "slide": 1, "html": "a.html", "focus": { "strategy": "switch" } }
    ]
  })");
  HTMLPPT_CHECK(result.config.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK(result.config->active.targets[0].focus.has_value());
  HTMLPPT_CHECK(result.config->active.targets[0].focus->timeout_ms.has_value());
  HTMLPPT_CHECK_EQ(*result.config->active.targets[0].focus->timeout_ms, 3500);
}

HTMLPPT_TEST_CASE(config_rejects_a_non_positive_focus_timeout) {
  const auto zero = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "targets": [{ "slide": 1, "html": "a.html",
                  "focus": { "timeoutMs": 0 } }]
  })");
  HTMLPPT_CHECK(HasError(zero));
  HTMLPPT_CHECK(HasIssueAt(zero, "targets[0].focus.timeoutMs"));

  const auto negative = Parse(R"({
    "version": 1,
    "deck": "deck-live.pptx",
    "defaults": { "focus": { "timeoutMs": -1 } },
    "targets": [{ "slide": 1, "html": "a.html" }]
  })");
  HTMLPPT_CHECK(HasError(negative));
  HTMLPPT_CHECK(HasIssueAt(negative, "defaults.focus.timeoutMs"));
}