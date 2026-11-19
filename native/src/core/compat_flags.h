#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace htmlppt {

inline constexpr std::string_view kLockFile = "watcher.lock";
inline constexpr std::string_view kCoverFadeFlag = "cover-fade.flag";
inline constexpr std::string_view kCoverReadyFlag = "cover-ready.flag";
inline constexpr std::string_view kCoverDoneFlag = "cover-done.flag";
inline constexpr std::string_view kCoverStopFlag = "cover-stop.flag";
inline constexpr std::string_view kCoverHideFlag = "cover-hide.flag";
inline constexpr std::string_view kAgentArmFlag = "agent-arm.flag";
inline constexpr std::string_view kAgentStopFlag = "agent-stop.flag";
inline constexpr std::string_view kReturnRequestFlag = "return-request.flag";
inline constexpr std::string_view kEdgeClosedFlag = "edge-closed.flag";
inline constexpr std::string_view kWatchLog = "watch-log.txt";
inline constexpr std::string_view kBringLog = "bring-log.txt";
inline constexpr std::string_view kCoverLog = "cover-log.txt";

struct CompatIdentity {
  std::string run_id;
  std::uint32_t pid = 0;
  std::string config_hash;
};

class CompatFlags {
 public:
  CompatFlags(std::filesystem::path base_dir, CompatIdentity identity);

  const CompatIdentity& identity() const { return identity_; }

  // The directory the legacy flag files live in. The flag mirror needs it to
  // spell the two paths the persistent focus agent writes.
  const std::filesystem::path& base_dir() const { return base_dir_; }

  bool WriteLock(std::string_view timestamp);
  bool WriteFlag(std::string_view name, std::string_view payload = {});
  std::string ReadFlag(std::string_view name) const;
  bool IsOwned(std::string_view name) const;
  bool DeleteOwned(std::string_view name);
  std::size_t DeleteAllOwned();

 private:
  std::filesystem::path Path(std::string_view name) const;
  std::filesystem::path OwnerPath(std::string_view name) const;
  bool WriteText(const std::filesystem::path& path, std::string_view text) const;
  std::string ReadText(const std::filesystem::path& path) const;

  std::filesystem::path base_dir_;
  CompatIdentity identity_;
};

class LegacyLog {
 public:
  explicit LegacyLog(std::filesystem::path base_dir);

  bool Append(std::string_view file, std::string_view line) const;

 private:
  std::filesystem::path base_dir_;
};

std::string UtcTimestamp();

} // namespace htmlppt
