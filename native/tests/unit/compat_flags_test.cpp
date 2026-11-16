#include "test_main.h"
#include "test_utils.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/compat_flags.h"

using htmlppt::CompatFlags;
using htmlppt::CompatIdentity;
using htmlppt::LegacyLog;
using htmlppt::test::TempDir;

namespace {

void WriteTextFile(const std::filesystem::path& path,
                   const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool IsIsoUtcTimestamp(const std::string& value) {
  if (value.size() != 20) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (index == 4 || index == 7) {
      if (character != '-') return false;
    } else if (index == 10) {
      if (character != 'T') return false;
    } else if (index == 13 || index == 16) {
      if (character != ':') return false;
    } else if (index == 19) {
      if (character != 'Z') return false;
    } else if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

} // namespace

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
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverFadeFlag),
                   std::string{"payload"});
  HTMLPPT_CHECK(flags.DeleteOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / "cover-fade.flag.owner"));
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
  HTMLPPT_CHECK(lock.find("\"updated\"") != std::string::npos);
  HTMLPPT_CHECK(lock.find("2026-11-15T00:00:00Z") != std::string::npos);
}

HTMLPPT_TEST_CASE(compat_delete_all_owned_flags) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  flags.WriteFlag(htmlppt::kCoverFadeFlag, "");
  flags.WriteFlag(htmlppt::kCoverReadyFlag, "");
  flags.WriteFlag(htmlppt::kAgentStopFlag, "");
  const std::size_t failures = flags.DeleteAllOwned();
  HTMLPPT_CHECK_EQ(failures, std::size_t{0});
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverReadyFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kAgentStopFlag));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / htmlppt::kCoverReadyFlag));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / htmlppt::kAgentStopFlag));
}

HTMLPPT_TEST_CASE(compat_delete_all_owned_reports_failures) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  const auto payload = dir.path() / htmlppt::kCoverFadeFlag;
  std::filesystem::create_directories(payload);
  WriteTextFile(payload / "blocker.txt", "blocked");
  WriteTextFile(dir.path() / "cover-fade.flag.owner", "run-a");

  const std::size_t failures = flags.DeleteAllOwned();
  HTMLPPT_CHECK_EQ(failures, std::size_t{1});
  HTMLPPT_CHECK(flags.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(std::filesystem::exists(payload));
}

HTMLPPT_TEST_CASE(compat_delete_owned_keeps_owner_when_payload_removal_fails) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  const auto payload = dir.path() / htmlppt::kCoverFadeFlag;
  std::filesystem::create_directories(payload);
  WriteTextFile(payload / "blocker.txt", "blocked");
  WriteTextFile(dir.path() / "cover-fade.flag.owner", "run-a");

  HTMLPPT_CHECK(!flags.DeleteOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(flags.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(std::filesystem::exists(
      dir.path() / "cover-fade.flag.owner"));
  HTMLPPT_CHECK(std::filesystem::exists(payload / "blocker.txt"));
}

HTMLPPT_TEST_CASE(compat_write_flag_skips_payload_when_owner_write_fails) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  std::filesystem::create_directories(
      dir.path() / "cover-fade.flag.owner");
  HTMLPPT_CHECK(!flags.WriteFlag(htmlppt::kCoverFadeFlag, "payload"));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / htmlppt::kCoverFadeFlag));
}

HTMLPPT_TEST_CASE(compat_write_flag_removes_owner_when_payload_write_fails) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  std::filesystem::create_directories(
      dir.path() / htmlppt::kCoverFadeFlag);
  HTMLPPT_CHECK(!flags.WriteFlag(htmlppt::kCoverFadeFlag, "payload"));
  HTMLPPT_CHECK(!std::filesystem::exists(
      dir.path() / "cover-fade.flag.owner"));
}

HTMLPPT_TEST_CASE(compat_empty_run_id_never_owns) {
  TempDir dir;
  WriteTextFile(dir.path() / htmlppt::kCoverFadeFlag, "legacy");
  CompatFlags flags(dir.path(), CompatIdentity{"", 0, ""});
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
  const std::size_t failures = flags.DeleteAllOwned();
  HTMLPPT_CHECK_EQ(failures, std::size_t{0});
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverFadeFlag),
                   std::string{"legacy"});
  HTMLPPT_CHECK(std::filesystem::exists(
      dir.path() / htmlppt::kCoverFadeFlag));
}

HTMLPPT_TEST_CASE(legacy_log_appends_timestamped_lines) {
  TempDir dir;
  LegacyLog log(dir.path());
  HTMLPPT_CHECK(log.Append(htmlppt::kWatchLog, "cover started"));
  HTMLPPT_CHECK(log.Append(htmlppt::kWatchLog, "cover stopped"));
  const auto path = dir.path() / htmlppt::kWatchLog;
  HTMLPPT_CHECK(std::filesystem::exists(path));
  const std::string text = ReadFile(path);
  HTMLPPT_CHECK(text.find("cover started") != std::string::npos);
  HTMLPPT_CHECK(text.find("cover stopped") != std::string::npos);
  const auto newline_count =
      static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
  HTMLPPT_CHECK_EQ(newline_count, std::size_t{2});
  HTMLPPT_CHECK(IsIsoUtcTimestamp(text.substr(0, 20)));
}

HTMLPPT_TEST_CASE(compat_utc_timestamp_is_non_empty) {
  const std::string timestamp = htmlppt::UtcTimestamp();
  HTMLPPT_CHECK(!timestamp.empty());
  HTMLPPT_CHECK(IsIsoUtcTimestamp(timestamp));
}
