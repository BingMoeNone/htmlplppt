#include "test_main.h"

#include <string>

#include <nlohmann/json.hpp>

#include "core/config_schema.h"

HTMLPPT_TEST_CASE(config_schema_is_valid_json) {
  const auto schema = nlohmann::json::parse(htmlppt::ConfigSchema());
  HTMLPPT_CHECK(schema.is_object());
  HTMLPPT_CHECK_EQ(schema["$schema"].get<std::string>(),
                   std::string{"https://json-schema.org/draft/2020-12/schema"});
  HTMLPPT_CHECK_EQ(schema["title"].get<std::string>(),
                   std::string{"htmlppt watchdog config"});
}

HTMLPPT_TEST_CASE(config_schema_declares_core_properties) {
  const auto schema = nlohmann::json::parse(htmlppt::ConfigSchema());
  HTMLPPT_CHECK(schema["properties"].contains("version"));
  HTMLPPT_CHECK(schema["properties"].contains("deck"));
  HTMLPPT_CHECK(schema["properties"].contains("targets"));
  HTMLPPT_CHECK(schema["properties"].contains("profiles"));
  HTMLPPT_CHECK(schema["properties"].contains("legacy"));
  HTMLPPT_CHECK(schema["$defs"].contains("target"));
  HTMLPPT_CHECK(schema["$defs"]["target"]["properties"].contains("html"));
  HTMLPPT_CHECK(schema["$defs"]["target"]["properties"].contains("edgeProfile"));
  HTMLPPT_CHECK(schema["$defs"]["target"]["properties"]["trigger"]["enum"].size() >= 2);
}

HTMLPPT_TEST_CASE(config_schema_rejects_unknown_top_level_fields) {
  auto schema = nlohmann::json::parse(htmlppt::ConfigSchema());
  HTMLPPT_CHECK(schema["additionalProperties"] == false);

  const auto names = {"cover", "edge", "display", "focus", "assets", "match",
                      "target", "defaults", "stage", "profile", "legacy"};
  for (const char* name : names) {
    HTMLPPT_CHECK(schema["$defs"].contains(name));
    const auto& definition = schema["$defs"].at(name);
    HTMLPPT_CHECK(definition.is_object());
    HTMLPPT_CHECK(definition.contains("properties"));
    HTMLPPT_CHECK(definition.at("additionalProperties") == false);
  }

  for (auto definition = schema["$defs"].begin();
       definition != schema["$defs"].end(); ++definition) {
    if (definition->is_object() && definition->contains("properties")) {
      HTMLPPT_CHECK(definition->contains("additionalProperties"));
      HTMLPPT_CHECK(definition->at("additionalProperties") == false);
    }
  }
}

HTMLPPT_TEST_CASE(config_schema_display_monitor_accepts_integer) {
  const auto schema = nlohmann::json::parse(htmlppt::ConfigSchema());
  const auto& monitor = schema["$defs"]["display"]["properties"]["monitor"];
  HTMLPPT_CHECK(monitor.contains("oneOf"));
  HTMLPPT_CHECK(monitor.at("oneOf").is_array());
  bool integer_branch = false;
  for (const auto& branch : monitor.at("oneOf")) {
    if (branch.contains("type") && branch.at("type") == "integer" &&
        branch.contains("minimum") && branch.at("minimum") == 1) {
      integer_branch = true;
    }
  }
  HTMLPPT_CHECK(integer_branch);
}
