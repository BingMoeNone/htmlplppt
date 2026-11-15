#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace htmlppt {

#ifdef _WIN32

class JobHandle {
 public:
  JobHandle() = default;
  ~JobHandle();

  JobHandle(const JobHandle&) = delete;
  JobHandle& operator=(const JobHandle&) = delete;
  JobHandle(JobHandle&& other) noexcept;
  JobHandle& operator=(JobHandle&& other) noexcept;

  bool Create();
  bool Assign(HANDLE process);
  bool Terminate();
  bool valid() const { return handle_ != nullptr; }
  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

class ProcessHandle {
 public:
  ProcessHandle() = default;
  ~ProcessHandle();

  ProcessHandle(const ProcessHandle&) = delete;
  ProcessHandle& operator=(const ProcessHandle&) = delete;
  ProcessHandle(ProcessHandle&& other) noexcept;
  ProcessHandle& operator=(ProcessHandle&& other) noexcept;

  // Launches the executable suspended and adds it to `job`. If the job
  // assignment fails, the process is terminated and the function returns
  // false. The caller must keep `job` alive until the process ends; closing
  // the job kills every process it owns.
  bool LaunchSuspended(const std::filesystem::path& exe,
                       const std::vector<std::wstring>& args,
                       JobHandle& job);
  bool Resume();
  bool Terminate();
  bool WaitForExit(std::uint32_t timeout_ms);
  std::uint32_t pid() const { return pid_; }
  HANDLE process() const { return process_; }

 private:
  void Close();
  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  std::uint32_t pid_ = 0;
};

bool IsProcessAlive(std::uint32_t pid);

#endif // _WIN32

} // namespace htmlppt
