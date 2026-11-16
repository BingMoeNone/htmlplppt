#include "core/compat_flags.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <utility>

#include <nlohmann/json.hpp>

namespace htmlppt {

CompatFlags::CompatFlags(std::filesystem::path base_dir,
                         CompatIdentity identity)
    : base_dir_(std::move(base_dir)), identity_(std::move(identity)) {}

std::filesystem::path CompatFlags::Path(std::string_view name) const {
  return base_dir_ / std::string(name);
}

std::filesystem::path CompatFlags::OwnerPath(std::string_view name) const {
  return base_dir_ / (std::string(name) + ".owner");
}

bool CompatFlags::WriteText(const std::filesystem::path& path,
                            std::string_view text) const {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  return output.good();
}

std::string CompatFlags::ReadText(const std::filesystem::path& path) const {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool CompatFlags::WriteFlag(std::string_view name, std::string_view payload) {
  if (!WriteText(OwnerPath(name), identity_.run_id)) return false;
  if (!WriteText(Path(name), payload)) {
    std::error_code error;
    std::filesystem::remove(OwnerPath(name), error);
    return false;
  }
  return true;
}

std::string CompatFlags::ReadFlag(std::string_view name) const {
  return ReadText(Path(name));
}

bool CompatFlags::IsOwned(std::string_view name) const {
  if (identity_.run_id.empty()) return false;
  return ReadText(OwnerPath(name)) == identity_.run_id;
}

bool CompatFlags::DeleteOwned(std::string_view name) {
  if (!IsOwned(name)) return false;
  std::error_code payload_error;
  std::filesystem::remove(Path(name), payload_error);
  if (payload_error) return false;
  std::error_code owner_error;
  std::filesystem::remove(OwnerPath(name), owner_error);
  return !owner_error;
}

std::size_t CompatFlags::DeleteAllOwned() {
  constexpr std::string_view kNames[] = {
      kLockFile,     kCoverFadeFlag,  kCoverReadyFlag, kCoverDoneFlag,
      kCoverStopFlag, kCoverHideFlag, kAgentArmFlag,   kAgentStopFlag,
      kReturnRequestFlag, kEdgeClosedFlag,
  };
  std::size_t failures = 0;
  for (const std::string_view name : kNames) {
    if (!IsOwned(name)) continue;
    if (!DeleteOwned(name)) ++failures;
  }
  return failures;
}

bool CompatFlags::WriteLock(std::string_view timestamp) {
  nlohmann::ordered_json line{
      {"run_id", identity_.run_id},
      {"pid", identity_.pid},
      {"config_hash", identity_.config_hash},
      {"updated", std::string(timestamp)},
  };
  return WriteFlag(kLockFile, line.dump());
}

LegacyLog::LegacyLog(std::filesystem::path base_dir)
    : base_dir_(std::move(base_dir)) {}

bool LegacyLog::Append(std::string_view file, std::string_view line) const {
  std::error_code error;
  std::filesystem::create_directories(base_dir_, error);
  std::ofstream output(base_dir_ / std::string(file),
                       std::ios::binary | std::ios::app);
  if (!output) return false;
  output << UtcTimestamp() << ' ' << line << '\n';
  output.close();
  return output.good();
}

std::string UtcTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &time);
#else
  gmtime_r(&time, &parts);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

} // namespace htmlppt
