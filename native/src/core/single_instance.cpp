#include "core/single_instance.h"

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace htmlppt {

std::string StableHash16(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char character : text) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
      << hash;
  return out.str();
}

InstanceIdentity MakeInstanceIdentity(const std::filesystem::path& config_dir,
                                      std::string_view config_hash) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(config_dir, error);
  if (error) canonical = config_dir;
  const std::string seed =
      canonical.generic_string() + "|" + std::string(config_hash);
  InstanceIdentity identity;
  identity.fingerprint = StableHash16(seed);
  identity.mutex_name = "Local\\htmlppt-watchdog-" + identity.fingerprint;
  return identity;
}

std::string FormatLockLine(const LockInfo& info) {
  nlohmann::ordered_json json;
  json["run_id"] = info.run_id;
  json["pid"] = info.pid;
  json["config_hash"] = info.config_hash;
  return json.dump();
}

std::optional<LockInfo> ParseLockLine(std::string_view line) {
  try {
    const auto json = nlohmann::json::parse(line.begin(), line.end());
    if (!json.is_object()) return std::nullopt;
    const auto run_id = json.find("run_id");
    const auto pid = json.find("pid");
    const auto config_hash = json.find("config_hash");
    if (run_id == json.end() || !run_id->is_string() ||
        run_id->get<std::string>().empty()) {
      return std::nullopt;
    }
    if (pid == json.end() || !pid->is_number_integer()) return std::nullopt;
    const auto pid_value = pid->get<std::int64_t>();
    if (pid_value <= 0 || pid_value > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    if (config_hash == json.end() || !config_hash->is_string()) {
      return std::nullopt;
    }
    LockInfo info;
    info.run_id = run_id->get<std::string>();
    info.pid = static_cast<int>(pid_value);
    info.config_hash = config_hash->get<std::string>();
    return info;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool IsLegacyLock(std::string_view line) {
  return !ParseLockLine(line).has_value();
}

} // namespace htmlppt
