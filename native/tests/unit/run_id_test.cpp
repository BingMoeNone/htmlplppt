#include "test_main.h"

#include <string>

#include "core/run_id.h"

using htmlppt::IsValidRunId;

HTMLPPT_TEST_CASE(run_id_accepts_letters_digits_and_safe_punctuation) {
  HTMLPPT_CHECK(IsValidRunId("run-1"));
  HTMLPPT_CHECK(IsValidRunId("ui-20261119-143000"));
  HTMLPPT_CHECK(IsValidRunId("a.b_c-9"));
  HTMLPPT_CHECK(IsValidRunId("A"));
}

HTMLPPT_TEST_CASE(run_id_rejects_path_separators_and_traversal) {
  HTMLPPT_CHECK(!IsValidRunId(""));
  HTMLPPT_CHECK(!IsValidRunId("."));
  HTMLPPT_CHECK(!IsValidRunId(".."));
  HTMLPPT_CHECK(!IsValidRunId("../escape"));
  HTMLPPT_CHECK(!IsValidRunId("a/b"));
  HTMLPPT_CHECK(!IsValidRunId("a\\b"));
  HTMLPPT_CHECK(!IsValidRunId("C:"));
  HTMLPPT_CHECK(!IsValidRunId("a b"));
}

HTMLPPT_TEST_CASE(run_id_rejects_overlong_ids) {
  HTMLPPT_CHECK(IsValidRunId(std::string(128, 'a')));
  HTMLPPT_CHECK(!IsValidRunId(std::string(129, 'a')));
}
