#include "test_main.h"

#include <string_view>

#include "core/version.h"

HTMLPPT_TEST_CASE(version_is_expected) {
  HTMLPPT_CHECK_EQ(htmlppt::version(), std::string_view{"0.1.0"});
}
