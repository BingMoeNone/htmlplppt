#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace htmlppt {

struct InstanceIdentity {
  std::string fingerprint;
  std::string mutex_name;
};

struct LockInfo {
  std::string run_id;
  int pid = 0;
  std::string config_hash;
};

std::string StableHash16(std::string_view text);
InstanceIdentity MakeInstanceIdentity(const std::filesystem::path& config_dir,
                                      std::string_view config_hash);
std::string FormatLockLine(const LockInfo& info);
std::optional<LockInfo> ParseLockLine(std::string_view line);
bool IsLegacyLock(std::string_view line);

} // namespace htmlppt
