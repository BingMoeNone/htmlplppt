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
