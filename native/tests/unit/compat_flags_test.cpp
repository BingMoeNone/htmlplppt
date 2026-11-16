#include "test_main.h"
#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/compat_flags.h"

using htmlppt::CompatFlags;
using htmlppt::CompatIdentity;
using htmlppt::LegacyLog;
using htmlppt::test::TempDir;

HTMLPPT_TEST_CASE(compat_owns_and_deletes_its_flags) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  HTMLPPT_CHECK(flags.WriteFlag(htmlppt::kCoverFadeFlag, "payload"));
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverFadeFlag),
                   std::string{"payload"});
  HTMLPPT_CHECK(flags.IsOwned(htmlppt::kCoverFadeFlag));

  CompatFlags other(dir.path(),
                    CompatIdentity{"run-b", 5678, "config-hash"});
  HTMLPPT_CHECK(!other.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!other.DeleteOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(flags.DeleteOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
}

HTMLPPT_TEST_CASE(compat_lock_line_contains_identity) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  HTMLPPT_CHECK(flags.WriteLock("2026-11-15T00:00:00Z"));
  const std::string lock = flags.ReadFlag(htmlppt::kLockFile);
  HTMLPPT_CHECK(lock.find("run-a") != std::string::npos);
  HTMLPPT_CHECK(lock.find("1234") != std::string::npos);
  HTMLPPT_CHECK(lock.find("config-hash") != std::string::npos);
}

HTMLPPT_TEST_CASE(compat_delete_all_owned_flags) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  flags.WriteFlag(htmlppt::kCoverFadeFlag, "");
  flags.WriteFlag(htmlppt::kCoverReadyFlag, "");
  flags.WriteFlag(htmlppt::kAgentStopFlag, "");
  flags.DeleteAllOwned();
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverReadyFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kAgentStopFlag));
}

HTMLPPT_TEST_CASE(legacy_log_appends_timestamped_lines) {
  TempDir dir;
  LegacyLog log(dir.path());
  HTMLPPT_CHECK(log.Append(htmlppt::kWatchLog, "cover started"));
  const auto path = dir.path() / htmlppt::kWatchLog;
  HTMLPPT_CHECK(std::filesystem::exists(path));
  const std::string text = [&] {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  }();
  HTMLPPT_CHECK(text.find("cover started") != std::string::npos);
  HTMLPPT_CHECK(text.find("T") != std::string::npos);
}

HTMLPPT_TEST_CASE(compat_utc_timestamp_is_non_empty) {
  HTMLPPT_CHECK(!htmlppt::UtcTimestamp().empty());
}
