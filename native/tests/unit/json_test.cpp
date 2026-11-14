#include "test_main.h"

#include <string>

#include <nlohmann/json.hpp>

HTMLPPT_TEST_CASE(json_parses_object) {
  const auto value = nlohmann::json::parse(R"({"a":1,"b":"x"})");
  HTMLPPT_CHECK_EQ(value["a"].get<int>(), 1);
  HTMLPPT_CHECK_EQ(value["b"].get<std::string>(), std::string{"x"});
}
