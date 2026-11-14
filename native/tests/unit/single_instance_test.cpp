#include "test_main.h"

#include <string>

#include "core/single_instance.h"

using htmlppt::FormatLockLine;
using htmlppt::IsLegacyLock;
using htmlppt::LockInfo;
using htmlppt::MakeInstanceIdentity;
using htmlppt::ParseLockLine;
using htmlppt::StableHash16;

HTMLPPT_TEST_CASE(instance_identity_is_stable_and_path_sensitive) {
  const auto first = MakeInstanceIdentity("/tmp/demo-a", "hash-1");
  const auto second = MakeInstanceIdentity("/tmp/demo-a", "hash-1");
  const auto third = MakeInstanceIdentity("/tmp/demo-b", "hash-1");
  HTMLPPT_CHECK_EQ(first.fingerprint, second.fingerprint);
  HTMLPPT_CHECK(first.fingerprint != third.fingerprint);
  HTMLPPT_CHECK_EQ(first.fingerprint.size(), std::size_t{16});
  HTMLPPT_CHECK_EQ(first.mutex_name,
                   std::string{"Local\\htmlppt-watchdog-"} + first.fingerprint);
}

HTMLPPT_TEST_CASE(stable_hash_is_16_hex_and_config_sensitive) {
  const auto first = StableHash16("alpha");
  const auto second = StableHash16("beta");
  HTMLPPT_CHECK_EQ(first.size(), std::size_t{16});
  HTMLPPT_CHECK(first != second);
}

HTMLPPT_TEST_CASE(lock_line_round_trips) {
  LockInfo info;
  info.run_id = "run-1";
  info.pid = 4242;
  info.config_hash = "abc123";
  const std::string line = FormatLockLine(info);
  const auto parsed = ParseLockLine(line);
  HTMLPPT_CHECK(parsed.has_value());
  HTMLPPT_CHECK_EQ(parsed->run_id, std::string{"run-1"});
  HTMLPPT_CHECK_EQ(parsed->pid, 4242);
  HTMLPPT_CHECK_EQ(parsed->config_hash, std::string{"abc123"});
  HTMLPPT_CHECK(!IsLegacyLock(line));
}

HTMLPPT_TEST_CASE(legacy_lock_detection) {
  HTMLPPT_CHECK(IsLegacyLock("not json"));
  HTMLPPT_CHECK(IsLegacyLock(R"({"pid": 42})"));
  HTMLPPT_CHECK(IsLegacyLock(R"({"run_id": "", "pid": 42})"));
  HTMLPPT_CHECK(!IsLegacyLock(R"({"run_id":"r","pid":42,"config_hash":"h"})"));
}
