#pragma once

#ifdef _WIN32

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "core/edge_controller.h"
#include "platform/win32/process_job.h"

namespace htmlppt {

class Win32EdgeProcess final : public EdgeProcess {
 public:
  Win32EdgeProcess(ProcessHandle process, JobHandle job);
  ~Win32EdgeProcess() override;

  Win32EdgeProcess(const Win32EdgeProcess&) = delete;
  Win32EdgeProcess& operator=(const Win32EdgeProcess&) = delete;

  std::uint32_t pid() const override;
  bool alive() const override;
  void terminate() override;
  bool release() override;

 private:
  ProcessHandle process_;
  JobHandle job_;
};

class Win32EdgeLauncher final : public EdgeLauncher {
 public:
  std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) override;
};

// Returns the first existing regular file from `candidates`, then falls back
// to SearchPathW for `name`.
std::optional<std::filesystem::path> FindExecutable(
    std::wstring_view name,
    const std::vector<std::filesystem::path>& candidates);

std::optional<std::filesystem::path> FindEdgeExecutable();

} // namespace htmlppt

#endif // _WIN32
