# C++ Watchdog Win32 Platform Primitives Implementation Plan (Plan 4B-1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement and verify the Windows platform primitives required by the supervisor: Job Object process ownership, named-pipe frame transport, shared-memory black-box storage, and MiniDump crash-dump writing.

**Architecture:** `platform/win32` owns the Windows API surface and is compiled only on WIN32. Each primitive is a small RAII class with a focused test executable (`htmlppt_win32_tests`) that runs on the real Windows kernel. The platform-neutral supervisor decision engine from Plan 4A stays untouched. Plan 4B-2 wires these primitives into the supervisor/worker hosts and CLI modes.

**Tech Stack:** C++20, Win32 (Job Objects, named pipes, file mappings, MiniDump), MSVC 19.51 via the Visual Studio 18 Build Tools, the existing `htmlppt_core` library and test harness.

## Global Constraints

- Target Windows 10/11 x64. This plan must also keep the Linux/WSL build and its 191 unit cases green.
- C++20; MSVC `/W4 /permissive- /utf-8 /WX`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only. Configuration content is UTF-8 at runtime.
- `core/` must not include Windows headers. All Win32 includes live under `src/platform/win32/` or guarded test files.
- Windows code is compiled and tested on the real Windows kernel through the Visual Studio Build Tools found in WSL; do not claim Windows verification from a Linux-only build.
- Named-pipe payloads use the Plan 4A frame format and CRC32. The shared black box uses the Plan 4A 96-byte `BlackboxRecord`.
- Job Objects must use `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`; named pipes and file mappings must be closed by RAII.
- Existing Linux suite: 191 unit cases, CTest 8/8, `npm test` 3/3 with `CHROME_BIN`.
- Windows gate after this plan: `htmlppt_core_tests` 189/189 (two pre-existing `journal_test.cpp` cases are excluded on Windows by `#ifndef _WIN32`), `htmlppt_win32_tests` 16/16, CTest 9/9, MSVC `/WX` clean.

## Windows Build Toolchain (available in this WSL environment)

```text
CMake: C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
CTest: C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe
Generator: Visual Studio 18 2026
MSVC: 19.51.36252.0 (14.51.36231)
Windows SDK: 10.0.26100.0
```

Run Windows builds from a Windows-visible copy to avoid UNC/MSBuild path issues:

```bash
WIN_SRC=/mnt/c/Temp/htmlplppt-win
rm -rf "$WIN_SRC" && mkdir -p "$WIN_SRC"
cp -r native "$WIN_SRC/native"
WIN_CMAKE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
WIN_CTEST="/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"
"$WIN_CMAKE" -S C:/Temp/htmlplppt-win/native -B C:/Temp/htmlplppt-win/build \
  -G "Visual Studio 18 2026" -A x64 -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
"$WIN_CMAKE" --build C:/Temp/htmlplppt-win/build --config Release --parallel
"$WIN_CTEST" --test-dir C:/Temp/htmlplppt-win/build -C Release --output-on-failure
```

The copied tree is disposable; re-copy `native` before each Windows gate.

## File Structure

Create:

- `native/src/platform/win32/process_job.h` - Job/process RAII, suspended launch, resume, terminate, liveness.
- `native/src/platform/win32/process_job.cpp` - Win32 Job Object and `CreateProcessW` implementation.
- `native/src/platform/win32/named_pipe.h` - pipe server/channel/client declarations.
- `native/src/platform/win32/named_pipe.cpp` - overlapped named-pipe transport using `EncodeFrame`/`DecodeFrame`.
- `native/src/platform/win32/shared_blackbox.h` - shared-memory black-box declaration.
- `native/src/platform/win32/shared_blackbox.cpp` - file mapping, header initialization, lock-free append/snapshot.
- `native/src/platform/win32/crash_dump.h` - MiniDump declaration.
- `native/src/platform/win32/crash_dump.cpp` - `MiniDumpWriteDump` implementation.
- `native/tests/integration/win32/process_job_test.cpp` - Job/process tests.
- `native/tests/integration/win32/named_pipe_test.cpp` - pipe tests.
- `native/tests/integration/win32/shared_blackbox_test.cpp` - shared-memory tests.
- `native/tests/integration/win32/crash_dump_test.cpp` - dump tests.

Modify:

- `native/CMakeLists.txt` - add the WIN32 library, the WIN32 test executable, and the `win32_tests` CTest entry.
- `native/README.md` - document the Windows primitives and the Windows build gate.
- `docs/superpowers/plans/2026-11-15-cpp-watchdog-win32-primitives.md` - as-built amendments after execution.

---

### Task 1: Job Object and Process Ownership

**Files:**
- Create: `native/src/platform/win32/process_job.h`
- Create: `native/src/platform/win32/process_job.cpp`
- Create: `native/tests/integration/win32/process_job_test.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: Win32 `CreateJobObjectW`, `SetInformationJobObject`, `AssignProcessToJobObject`, `CreateProcessW`, `ResumeThread`, `TerminateJobObject`, `TerminateProcess`, `WaitForSingleObject`.
- Produces:
  - `class JobHandle { bool Create(); bool Assign(HANDLE); bool Terminate(); bool valid() const; HANDLE get() const; }`
  - `class ProcessHandle { bool LaunchSuspended(const std::filesystem::path&, const std::vector<std::wstring>&, JobHandle&); bool Resume(); bool Terminate(); bool WaitForExit(std::uint32_t timeout_ms); std::uint32_t pid() const; HANDLE process() const; }`
  - `bool IsProcessAlive(std::uint32_t pid);`

- [ ] **Step 1: Write the failing Windows Job tests**

Create `native/tests/integration/win32/process_job_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/win32/process_job.h"

using htmlppt::IsProcessAlive;
using htmlppt::JobHandle;
using htmlppt::ProcessHandle;

namespace {

std::filesystem::path CmdExe() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"ComSpec", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return std::filesystem::path(buffer);
  return std::filesystem::path(L"C:\\Windows\\System32\\cmd.exe");
}

std::vector<std::wstring> SleepArgs() {
  return {L"/c", L"ping", L"-n", L"30", L"127.0.0.1"};
}

void LaunchChild(JobHandle& job, ProcessHandle& process) {
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(process.LaunchSuspended(CmdExe(), SleepArgs(), job));
  HTMLPPT_CHECK(process.Resume());
}

} // namespace

HTMLPPT_TEST_CASE(win32_job_create_assign_and_terminate) {
  JobHandle job;
  ProcessHandle process;
  LaunchChild(job, process);
  HTMLPPT_CHECK(process.pid() != 0);
  HTMLPPT_CHECK(IsProcessAlive(process.pid()));
  HTMLPPT_CHECK(job.Terminate());
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(process.pid()));
}

HTMLPPT_TEST_CASE(win32_process_resume_and_terminate) {
  JobHandle job;
  ProcessHandle process;
  LaunchChild(job, process);
  HTMLPPT_CHECK(process.Terminate());
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(process.pid()));
}

HTMLPPT_TEST_CASE(win32_process_resume_runs_child) {
  JobHandle job;
  ProcessHandle process;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(process.LaunchSuspended(
      CmdExe(), {L"/c", L"exit", L"0"}, job));
  HTMLPPT_CHECK(process.Resume());
  HTMLPPT_CHECK(process.WaitForExit(5000));
}

HTMLPPT_TEST_CASE(win32_job_kill_on_close) {
  ProcessHandle process;
  std::uint32_t pid = 0;
  {
    JobHandle job;
    LaunchChild(job, process);
    pid = process.pid();
    HTMLPPT_CHECK(job.valid());
    HTMLPPT_CHECK(IsProcessAlive(pid));
  }
  HTMLPPT_CHECK(process.WaitForExit(5000));
  HTMLPPT_CHECK(!IsProcessAlive(pid));
}

HTMLPPT_TEST_CASE(win32_is_process_alive_reports_current_process) {
  HTMLPPT_CHECK(IsProcessAlive(GetCurrentProcessId()));
  HTMLPPT_CHECK(!IsProcessAlive(0xFFFFFFFFu));
}

#endif // _WIN32
```

- [ ] **Step 2: Add the Windows library and test target**

In `native/CMakeLists.txt`, after the `htmlppt_core` target and before the existing executable, add:

```cmake
if (WIN32)
  file(GLOB HTMLPPT_WIN32_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32/*.cpp")
  add_library(htmlppt_win32 STATIC ${HTMLPPT_WIN32_SOURCES})
  target_link_libraries(htmlppt_win32 PUBLIC htmlppt_core dbghelp)
  htmlppt_enable_warnings(htmlppt_win32)
endif()
```

Inside `if (HTMLPPT_BUILD_TESTS)`, after the existing CLI tests, add:

```cmake
  if (WIN32)
    file(GLOB HTMLPPT_WIN32_TEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/win32/*.cpp")
    add_executable(htmlppt_win32_tests
      ${HTMLPPT_WIN32_TEST_SOURCES}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_main.cpp")
    target_include_directories(htmlppt_win32_tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(htmlppt_win32_tests PRIVATE htmlppt_win32)
    htmlppt_enable_warnings(htmlppt_win32_tests)
    add_test(NAME win32_tests COMMAND htmlppt_win32_tests)
    set_tests_properties(win32_tests PROPERTIES TIMEOUT 120)
  endif()
```

Run the Windows build block from the top of this plan. Expected: compilation fails because `platform/win32/process_job.h` does not exist.

- [ ] **Step 3: Add the Job/process header and implementation**

Create `native/src/platform/win32/process_job.h`:

```cpp
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
```

Create `native/src/platform/win32/process_job.cpp`:

```cpp
#include "platform/win32/process_job.h"

#ifdef _WIN32

#include <utility>

namespace htmlppt {
namespace {

std::wstring Quote(const std::wstring& value) {
  if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
  return L"\"" + value + L"\"";
}

} // namespace

JobHandle::~JobHandle() {
  if (handle_ != nullptr) CloseHandle(handle_);
}

JobHandle::JobHandle(JobHandle&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

JobHandle& JobHandle::operator=(JobHandle&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) CloseHandle(handle_);
    handle_ = std::exchange(other.handle_, nullptr);
  }
  return *this;
}

bool JobHandle::Create() {
  if (handle_ != nullptr) return true;
  handle_ = CreateJobObjectW(nullptr, nullptr);
  if (handle_ == nullptr) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (SetInformationJobObject(handle_, JobObjectExtendedLimitInformation,
                              &limits, sizeof(limits)) == 0) {
    CloseHandle(handle_);
    handle_ = nullptr;
    return false;
  }
  return true;
}

bool JobHandle::Assign(HANDLE process) {
  return handle_ != nullptr && process != nullptr &&
         AssignProcessToJobObject(handle_, process) != 0;
}

bool JobHandle::Terminate() {
  return handle_ != nullptr && TerminateJobObject(handle_, 1) != 0;
}

ProcessHandle::~ProcessHandle() { Close(); }

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)),
      thread_(std::exchange(other.thread_, nullptr)),
      pid_(std::exchange(other.pid_, 0)) {}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
  if (this != &other) {
    Close();
    process_ = std::exchange(other.process_, nullptr);
    thread_ = std::exchange(other.thread_, nullptr);
    pid_ = std::exchange(other.pid_, 0);
  }
  return *this;
}

void ProcessHandle::Close() {
  if (thread_ != nullptr) {
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (process_ != nullptr) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  pid_ = 0;
}

bool ProcessHandle::LaunchSuspended(
    const std::filesystem::path& exe, const std::vector<std::wstring>& args,
    JobHandle& job) {
  if (!job.valid()) return false;
  Close();
  std::wstring command_line = Quote(exe.wstring());
  for (const auto& arg : args) {
    command_line += L" ";
    command_line += Quote(arg);
  }
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const BOOL created = CreateProcessW(
      exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
      nullptr, &startup, &information);
  if (created == FALSE) return false;
  process_ = information.hProcess;
  thread_ = information.hThread;
  pid_ = static_cast<std::uint32_t>(information.dwProcessId);
  if (!job.Assign(process_)) {
    TerminateProcess(process_, 1);
    Close();
    return false;
  }
  return true;
}

bool ProcessHandle::Resume() {
  if (thread_ == nullptr) return false;
  const DWORD result = ResumeThread(thread_);
  CloseHandle(thread_);
  thread_ = nullptr;
  return result != static_cast<DWORD>(-1);
}

bool ProcessHandle::Terminate() {
  return process_ != nullptr && TerminateProcess(process_, 1) != 0;
}

bool ProcessHandle::WaitForExit(std::uint32_t timeout_ms) {
  if (process_ == nullptr) return false;
  return WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
}

bool IsProcessAlive(std::uint32_t pid) {
  if (pid == 0) return false;
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, pid);
  if (process == nullptr) return false;
  const DWORD result = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return result == WAIT_TIMEOUT;
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Build and run the Windows tests**

Run the Windows build block. Expected: `htmlppt_win32_tests` builds; `win32_tests` passes 5/5 (5 Job cases); `htmlppt_core_tests` 189/189 (two pre-existing journal cases are excluded on Windows); Windows CTest 9/9. Also run the Linux gate:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: Linux 191/191 unit cases, CTest 8/8.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/process_job.h \
  native/src/platform/win32/process_job.cpp \
  native/tests/integration/win32/process_job_test.cpp native/CMakeLists.txt
git commit -m "feat: add win32 job and process ownership"
```

---

### Task 2: Named-Pipe Frame Transport

**Files:**
- Create: `native/src/platform/win32/named_pipe.h`
- Create: `native/src/platform/win32/named_pipe.cpp`
- Create: `native/tests/integration/win32/named_pipe_test.cpp`

**Interfaces:**
- Consumes: Plan 4A `Frame`, `EncodeFrame`, `DecodeFrame`, `DecodeStatus`; Win32 named pipes and overlapped I/O.
- Produces:
  - `class NamedPipeServer { bool Create(const std::wstring& name); std::unique_ptr<NamedPipeChannel> Accept(Duration timeout); void Close(); }`
  - `class NamedPipeChannel { bool Send(const Frame& frame); std::optional<Frame> Receive(Duration timeout); void Close(); }`
  - `class NamedPipeClient { bool Connect(const std::wstring& name, Duration timeout); bool Send(const Frame& frame); std::optional<Frame> Receive(Duration timeout); void Close(); }`

- [ ] **Step 1: Write the failing pipe tests**

Create `native/tests/integration/win32/named_pipe_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/supervisor_protocol.h"
#include "platform/win32/named_pipe.h"

using htmlppt::Duration;
using htmlppt::Frame;
using htmlppt::FrameType;
using htmlppt::NamedPipeClient;
using htmlppt::NamedPipeServer;

namespace {

std::wstring UniquePipeName() {
  static std::atomic<std::uint32_t> counter{0};
  return L"\\\\.\\pipe\\htmlppt-test-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter++);
}

Frame MakeFrame(std::uint32_t sequence, const std::string& text) {
  Frame frame;
  frame.header.sequence = sequence;
  frame.header.type = FrameType::Log;
  frame.payload.assign(text.begin(), text.end());
  return frame;
}

} // namespace

HTMLPPT_TEST_CASE(win32_named_pipe_round_trip) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000}) &&
                client.Send(MakeFrame(7, "hello"));
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  const auto frame = channel->Receive(Duration{3000});
  HTMLPPT_CHECK(frame.has_value());
  HTMLPPT_CHECK_EQ(frame->header.sequence, std::uint32_t{7});
  HTMLPPT_CHECK_EQ(std::string(frame->payload.begin(), frame->payload.end()),
                   std::string{"hello"});
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_multiple_frames) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000}) &&
                client.Send(MakeFrame(1, "one")) &&
                client.Send(MakeFrame(2, "two")) &&
                client.Send(MakeFrame(3, "three"));
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  for (std::uint32_t i = 1; i <= 3; ++i) {
    const auto frame = channel->Receive(Duration{3000});
    HTMLPPT_CHECK(frame.has_value());
    HTMLPPT_CHECK_EQ(frame->header.sequence, i);
  }
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_disconnect_returns_empty) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000});
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  const auto frame = channel->Receive(Duration{300});
  HTMLPPT_CHECK(!frame.has_value());
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_connect_timeout) {
  NamedPipeClient client;
  HTMLPPT_CHECK(!client.Connect(L"\\\\.\\pipe\\htmlppt-missing-pipe",
                                Duration{100}));
}

HTMLPPT_TEST_CASE(win32_named_pipe_receive_timeout_is_safe) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  std::atomic<bool> close_client{false};
  std::atomic<bool> client_ok{false};
  std::thread client_thread([&] {
    NamedPipeClient client;
    client_ok = client.Connect(name, Duration{3000});
    while (!close_client.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.Close();
  });
  auto channel = server.Accept(Duration{3000});
  const auto frame =
      channel ? channel->Receive(Duration{100}) : std::nullopt;
  close_client = true;
  client_thread.join();
  HTMLPPT_CHECK(channel != nullptr);
  HTMLPPT_CHECK(client_ok.load());
  HTMLPPT_CHECK(!frame.has_value());
  channel->Close();
  server.Close();
}

HTMLPPT_TEST_CASE(win32_named_pipe_accept_failure_does_not_wait) {
  NamedPipeServer server;
  const std::wstring name = UniquePipeName();
  HTMLPPT_CHECK(server.Create(name));
  NamedPipeClient client;
  HTMLPPT_CHECK(client.Connect(name, Duration{3000}));
  client.Close();
  const auto start = std::chrono::steady_clock::now();
  auto channel = server.Accept(Duration{2000});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  HTMLPPT_CHECK(elapsed < std::chrono::milliseconds(1500));
  if (channel != nullptr) channel->Close();
  server.Close();
}

#endif // _WIN32
```

- [ ] **Step 2: Build and verify the tests fail**

Run the Windows build block. Expected: compilation fails because `platform/win32/named_pipe.h` does not exist.

- [ ] **Step 3: Add the named-pipe header and implementation**

Create `native/src/platform/win32/named_pipe.h`:

```cpp
#pragma once

#include <memory>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/clock.h"
#include "core/supervisor_protocol.h"

namespace htmlppt {

#ifdef _WIN32

class NamedPipeChannel {
 public:
  NamedPipeChannel() = default;
  ~NamedPipeChannel();
  NamedPipeChannel(const NamedPipeChannel&) = delete;
  NamedPipeChannel& operator=(const NamedPipeChannel&) = delete;

  // Uses a fixed 3-second timeout. On failure the stream may contain a partial
  // frame, so the channel must be closed.
  bool Send(const Frame& frame);
  std::optional<Frame> Receive(Duration timeout);
  void Close();

 private:
  friend class NamedPipeServer;
  explicit NamedPipeChannel(HANDLE pipe) : pipe_(pipe) {}
  bool ReadExact(std::uint8_t* buffer, std::size_t size, Duration timeout);
  bool WriteExact(const std::uint8_t* buffer, std::size_t size,
                  Duration timeout);
  HANDLE pipe_ = nullptr;
};

class NamedPipeServer {
 public:
  NamedPipeServer() = default;
  ~NamedPipeServer();
  NamedPipeServer(const NamedPipeServer&) = delete;
  NamedPipeServer& operator=(const NamedPipeServer&) = delete;

  bool Create(const std::wstring& name);
  // On failure or timeout the server handle is closed; the caller must call
  // Create again before another Accept.
  std::unique_ptr<NamedPipeChannel> Accept(Duration timeout);
  void Close();

 private:
  std::wstring name_;
  HANDLE pipe_ = nullptr;
};

class NamedPipeClient {
 public:
  NamedPipeClient() = default;
  ~NamedPipeClient();
  NamedPipeClient(const NamedPipeClient&) = delete;
  NamedPipeClient& operator=(const NamedPipeClient&) = delete;

  bool Connect(const std::wstring& name, Duration timeout);
  // Same fixed 3-second timeout and partial-frame contract as
  // NamedPipeChannel::Send.
  bool Send(const Frame& frame);
  // Same timeout and nullopt contract as NamedPipeChannel::Receive.
  std::optional<Frame> Receive(Duration timeout);
  void Close();

 private:
  bool ReadExact(std::uint8_t* buffer, std::size_t size, Duration timeout);
  bool WriteExact(const std::uint8_t* buffer, std::size_t size,
                  Duration timeout);
  HANDLE pipe_ = nullptr;
};

#endif // _WIN32

} // namespace htmlppt
```

Create `native/src/platform/win32/named_pipe.cpp`:

```cpp
#include "platform/win32/named_pipe.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

namespace htmlppt {
namespace {

constexpr DWORD kPipeBufferSize = 64 * 1024;

bool WaitFor(HANDLE event, Duration timeout) {
  const auto count = timeout.count();
  const DWORD wait_ms = count <= 0 ? 0 : static_cast<DWORD>(count);
  return WaitForSingleObject(event, wait_ms) == WAIT_OBJECT_0;
}

std::uint32_t ReadPayloadLength(const std::uint8_t* header) {
  return static_cast<std::uint32_t>(header[22]) |
         (static_cast<std::uint32_t>(header[23]) << 8) |
         (static_cast<std::uint32_t>(header[24]) << 16) |
         (static_cast<std::uint32_t>(header[25]) << 24);
}

bool ReadPipeExact(HANDLE pipe, std::uint8_t* buffer, std::size_t size,
                   Duration timeout) {
  std::size_t offset = 0;
  while (offset < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    DWORD read = 0;
    const BOOL started =
        ReadFile(pipe, buffer + offset, static_cast<DWORD>(size - offset),
                 nullptr, &overlapped);
    bool ok = started != FALSE;
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
      CloseHandle(overlapped.hEvent);
      return false;
    }
    if (!ok) {
      ok = WaitFor(overlapped.hEvent, timeout) != FALSE;
      if (!ok) {
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        return false;
      }
      ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE) != FALSE;
    } else {
      ok = GetOverlappedResult(pipe, &overlapped, &read, FALSE) != FALSE;
    }
    CloseHandle(overlapped.hEvent);
    if (!ok || read == 0) return false;
    offset += read;
  }
  return true;
}

bool WritePipeExact(HANDLE pipe, const std::uint8_t* buffer, std::size_t size,
                    Duration timeout) {
  std::size_t offset = 0;
  while (offset < size) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) return false;
    DWORD written = 0;
    const BOOL started =
        WriteFile(pipe, buffer + offset, static_cast<DWORD>(size - offset),
                  nullptr, &overlapped);
    bool ok = started != FALSE;
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
      CloseHandle(overlapped.hEvent);
      return false;
    }
    if (!ok) {
      ok = WaitFor(overlapped.hEvent, timeout) != FALSE;
      if (!ok) {
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        return false;
      }
      ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
    } else {
      ok = GetOverlappedResult(pipe, &overlapped, &written, FALSE) != FALSE;
    }
    CloseHandle(overlapped.hEvent);
    if (!ok || written == 0) return false;
    offset += written;
  }
  return true;
}

std::optional<Frame> ReceiveFrame(HANDLE pipe, Duration timeout) {
  std::uint8_t header[kFrameHeaderSize] = {};
  if (!ReadPipeExact(pipe, header, sizeof(header), timeout)) return std::nullopt;
  const std::uint32_t length = ReadPayloadLength(header);
  if (length > kMaxFramePayload) return std::nullopt;
  std::vector<std::uint8_t> bytes(kFrameHeaderSize + length, 0);
  std::copy(header, header + kFrameHeaderSize, bytes.begin());
  if (length > 0 &&
      !ReadPipeExact(pipe, bytes.data() + kFrameHeaderSize, length, timeout)) {
    return std::nullopt;
  }
  const auto decoded = DecodeFrame(bytes);
  if (decoded.status != DecodeStatus::Ok) return std::nullopt;
  return decoded.frame;
}

} // namespace

NamedPipeChannel::~NamedPipeChannel() { Close(); }

void NamedPipeChannel::Close() {
  if (pipe_ != nullptr) {
    DisconnectNamedPipe(pipe_);
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
}

bool NamedPipeChannel::ReadExact(std::uint8_t* buffer, std::size_t size,
                                 Duration timeout) {
  return ReadPipeExact(pipe_, buffer, size, timeout);
}

bool NamedPipeChannel::WriteExact(const std::uint8_t* buffer, std::size_t size,
                                  Duration timeout) {
  return WritePipeExact(pipe_, buffer, size, timeout);
}

bool NamedPipeChannel::Send(const Frame& frame) {
  const auto bytes = EncodeFrame(frame);
  if (bytes.empty()) return false;
  return WriteExact(bytes.data(), bytes.size(), Duration{3000});
}

std::optional<Frame> NamedPipeChannel::Receive(Duration timeout) {
  return ReceiveFrame(pipe_, timeout);
}

NamedPipeServer::~NamedPipeServer() { Close(); }

void NamedPipeServer::Close() {
  if (pipe_ != nullptr) {
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
  name_.clear();
}

bool NamedPipeServer::Create(const std::wstring& name) {
  Close();
  pipe_ = CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kPipeBufferSize,
      kPipeBufferSize, 0, nullptr);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    pipe_ = nullptr;
    return false;
  }
  name_ = name;
  return true;
}

std::unique_ptr<NamedPipeChannel> NamedPipeServer::Accept(Duration timeout) {
  if (pipe_ == nullptr) return nullptr;
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlapped.hEvent == nullptr) return nullptr;
  BOOL connected = ConnectNamedPipe(pipe_, &overlapped);
  bool ok = connected != FALSE;
  if (!ok) {
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
      ok = true;
    } else if (error == ERROR_IO_PENDING) {
      ok = WaitFor(overlapped.hEvent, timeout) != FALSE;
      if (!ok) {
        CancelIoEx(pipe_, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe_, &overlapped, &ignored, TRUE);
        CloseHandle(overlapped.hEvent);
        Close();
        return nullptr;
      }
      DWORD transferred = 0;
      ok = GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) != FALSE;
    } else {
      CloseHandle(overlapped.hEvent);
      Close();
      return nullptr;
    }
  }
  CloseHandle(overlapped.hEvent);
  if (!ok) {
    Close();
    return nullptr;
  }
  HANDLE accepted = pipe_;
  pipe_ = nullptr;
  return std::unique_ptr<NamedPipeChannel>(new NamedPipeChannel(accepted));
}

NamedPipeClient::~NamedPipeClient() { Close(); }

void NamedPipeClient::Close() {
  if (pipe_ != nullptr) {
    CloseHandle(pipe_);
    pipe_ = nullptr;
  }
}

bool NamedPipeClient::Connect(const std::wstring& name, Duration timeout) {
  Close();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout.count());
  for (;;) {
    pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    const DWORD error = GetLastError();
    pipe_ = nullptr;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(name.c_str(), 50);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

bool NamedPipeClient::ReadExact(std::uint8_t* buffer, std::size_t size,
                                Duration timeout) {
  return ReadPipeExact(pipe_, buffer, size, timeout);
}

bool NamedPipeClient::WriteExact(const std::uint8_t* buffer, std::size_t size,
                                 Duration timeout) {
  return WritePipeExact(pipe_, buffer, size, timeout);
}

bool NamedPipeClient::Send(const Frame& frame) {
  const auto bytes = EncodeFrame(frame);
  if (bytes.empty()) return false;
  return WriteExact(bytes.data(), bytes.size(), Duration{3000});
}

std::optional<Frame> NamedPipeClient::Receive(Duration timeout) {
  return ReceiveFrame(pipe_, timeout);
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Build and run the Windows tests**

Run the Windows build block. Expected: `htmlppt_win32_tests` 11/11 (5 Job + 6 pipe); `htmlppt_core_tests` 189/189; Windows CTest 9/9. Run the Linux gate; expected 191/191, CTest 8/8.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/named_pipe.h \
  native/src/platform/win32/named_pipe.cpp \
  native/tests/integration/win32/named_pipe_test.cpp
git commit -m "feat: add win32 named-pipe frame transport"
```

---

### Task 3: Shared-Memory Black Box

**Files:**
- Create: `native/src/platform/win32/shared_blackbox.h`
- Create: `native/src/platform/win32/shared_blackbox.cpp`
- Create: `native/tests/integration/win32/shared_blackbox_test.cpp`

**Interfaces:**
- Consumes: Plan 4A `BlackboxRecord`; Win32 file mappings.
- Produces:
  - `class SharedBlackbox { static std::unique_ptr<SharedBlackbox> Create(const std::wstring& name, std::size_t capacity); static std::unique_ptr<SharedBlackbox> Open(const std::wstring& name); void Append(const BlackboxRecord&); std::vector<BlackboxRecord> Snapshot() const; std::uint64_t appended() const; std::uint64_t dropped() const; std::size_t capacity() const; bool valid() const; }`

- [ ] **Step 1: Write the failing shared-memory tests**

Create `native/tests/integration/win32/shared_blackbox_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "core/blackbox.h"
#include "platform/win32/shared_blackbox.h"

using htmlppt::BlackboxEvent;
using htmlppt::BlackboxRecord;
using htmlppt::SharedBlackbox;

namespace {

std::wstring UniqueMappingName() {
  static std::atomic<std::uint32_t> counter{0};
  return L"Local\\htmlppt-test-blackbox-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter++);
}

BlackboxRecord MakeRecord(std::int64_t timestamp) {
  BlackboxRecord record;
  record.timestamp_ms = timestamp;
  record.event = BlackboxEvent::Event;
  return record;
}

} // namespace

HTMLPPT_TEST_CASE(win32_shared_blackbox_round_trip) {
  const std::wstring name = UniqueMappingName();
  auto writer = SharedBlackbox::Create(name, 8);
  HTMLPPT_CHECK(writer != nullptr);
  writer->Append(MakeRecord(10));
  writer->Append(MakeRecord(20));
  writer->Append(MakeRecord(30));
  HTMLPPT_CHECK_EQ(writer->appended(), std::uint64_t{3});
  auto reader = SharedBlackbox::Open(name);
  HTMLPPT_CHECK(reader != nullptr);
  const auto snapshot = reader->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(snapshot[0].timestamp_ms, std::int64_t{10});
  HTMLPPT_CHECK_EQ(snapshot[2].sequence, std::uint64_t{3});
  reader->Append(MakeRecord(99));
  HTMLPPT_CHECK_EQ(reader->Snapshot().size(), std::size_t{3});
}

HTMLPPT_TEST_CASE(win32_shared_blackbox_wraps_and_counts_dropped) {
  const std::wstring name = UniqueMappingName();
  auto box = SharedBlackbox::Create(name, 3);
  HTMLPPT_CHECK(box != nullptr);
  box->Append(MakeRecord(10));
  box->Append(MakeRecord(20));
  box->Append(MakeRecord(30));
  box->Append(MakeRecord(40));
  const auto snapshot = box->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(snapshot[0].timestamp_ms, std::int64_t{20});
  HTMLPPT_CHECK_EQ(snapshot[2].timestamp_ms, std::int64_t{40});
  HTMLPPT_CHECK_EQ(box->dropped(), std::uint64_t{1});
}

HTMLPPT_TEST_CASE(win32_shared_blackbox_open_missing_returns_null) {
  const std::wstring name = UniqueMappingName();
  HTMLPPT_CHECK(SharedBlackbox::Open(name) == nullptr);
}

#endif // _WIN32
```

- [ ] **Step 2: Build and verify the tests fail**

Run the Windows build block. Expected: compilation fails because `platform/win32/shared_blackbox.h` does not exist.

- [ ] **Step 3: Add the shared black-box header and implementation**

Create `native/src/platform/win32/shared_blackbox.h`:

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/blackbox.h"

namespace htmlppt {

#ifdef _WIN32

// Shared-memory black-box ring. Append is single-writer: the caller must
// serialize all writes (Plan 4B-2 will use one writer thread or an MPSC
// queue). Snapshot stays within the mapping during a concurrent wrap, but the
// returned records may be torn or out of order and are diagnostic only. Create
// returns a writable mapping; Open returns a read-only mapping whose Append is
// a no-op.
class SharedBlackbox {
 public:
  static std::unique_ptr<SharedBlackbox> Create(const std::wstring& name,
                                                std::size_t capacity);
  static std::unique_ptr<SharedBlackbox> Open(const std::wstring& name);
  ~SharedBlackbox();

  SharedBlackbox(const SharedBlackbox&) = delete;
  SharedBlackbox& operator=(const SharedBlackbox&) = delete;

  void Append(const BlackboxRecord& record);
  std::vector<BlackboxRecord> Snapshot() const;
  std::uint64_t appended() const;
  std::uint64_t dropped() const;
  std::size_t capacity() const { return capacity_; }
  bool valid() const { return header_ != nullptr; }

 private:
  struct Header {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t record_size;
    std::uint32_t capacity;
    std::uint32_t reserved;
    std::atomic<std::uint64_t> appended;
    std::atomic<std::uint64_t> dropped;
  };

  SharedBlackbox(HANDLE mapping, void* view, std::size_t capacity,
                 bool writable);
  void Attach(std::size_t capacity);

  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  Header* header_ = nullptr;
  BlackboxRecord* records_ = nullptr;
  std::size_t capacity_ = 0;
  bool writable_ = false;
};

#endif // _WIN32

} // namespace htmlppt
```

Create `native/src/platform/win32/shared_blackbox.cpp`:

```cpp
#include "platform/win32/shared_blackbox.h"

#ifdef _WIN32

#include <algorithm>
#include <cstdint>
#include <new>

namespace htmlppt {
namespace {

constexpr std::uint32_t kBlackboxMagic =
    0x424B4C42u;  // "BLKB" in little-endian memory
constexpr std::uint16_t kBlackboxVersion = 1;

static_assert(sizeof(BlackboxRecord) == 96, "shared blackbox record layout");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "shared blackbox atomics must be lock-free");

} // namespace

SharedBlackbox::SharedBlackbox(HANDLE mapping, void* view,
                               std::size_t capacity, bool writable)
    : mapping_(mapping),
      view_(view),
      capacity_(capacity),
      writable_(writable) {
  static_assert(sizeof(Header) == 32, "shared blackbox header layout");
  Attach(capacity);
}

void SharedBlackbox::Attach(std::size_t capacity) {
  header_ = static_cast<Header*>(view_);
  records_ = reinterpret_cast<BlackboxRecord*>(
      static_cast<std::uint8_t*>(view_) + sizeof(Header));
  capacity_ = capacity;
}

SharedBlackbox::~SharedBlackbox() {
  if (view_ != nullptr) UnmapViewOfFile(view_);
  if (mapping_ != nullptr) CloseHandle(mapping_);
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::Create(
    const std::wstring& name, std::size_t capacity) {
  if (capacity == 0 ||
      capacity > static_cast<std::size_t>(0xFFFFFFFFu)) {
    return nullptr;
  }
  const std::uint64_t bytes =
      sizeof(Header) + static_cast<std::uint64_t>(capacity) *
                           sizeof(BlackboxRecord);
  SetLastError(0);
  HANDLE mapping = CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      static_cast<DWORD>(bytes >> 32),
      static_cast<DWORD>(bytes & 0xFFFFFFFFu), name.c_str());
  if (mapping == nullptr) return nullptr;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mapping);
    return nullptr;
  }
  void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                             static_cast<SIZE_T>(bytes));
  if (view == nullptr) {
    CloseHandle(mapping);
    return nullptr;
  }
  auto box = std::unique_ptr<SharedBlackbox>(
      new SharedBlackbox(mapping, view, capacity, true));
  new (box->header_) Header{};
  box->header_->magic = kBlackboxMagic;
  box->header_->version = kBlackboxVersion;
  box->header_->record_size = static_cast<std::uint16_t>(sizeof(BlackboxRecord));
  box->header_->capacity = static_cast<std::uint32_t>(capacity);
  box->header_->appended.store(0, std::memory_order_relaxed);
  box->header_->dropped.store(0, std::memory_order_relaxed);
  return box;
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::Open(const std::wstring& name) {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (mapping == nullptr) return nullptr;
  void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(mapping);
    return nullptr;
  }
  auto* header = static_cast<Header*>(view);
  if (header->magic != kBlackboxMagic ||
      header->version != kBlackboxVersion ||
      header->record_size != sizeof(BlackboxRecord) ||
      header->capacity == 0) {
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return nullptr;
  }
  return std::unique_ptr<SharedBlackbox>(
      new SharedBlackbox(mapping, view, header->capacity, false));
}

void SharedBlackbox::Append(const BlackboxRecord& record) {
  if (!writable_ || header_ == nullptr || records_ == nullptr ||
      capacity_ == 0) {
    return;
  }
  const std::uint64_t index =
      header_->appended.load(std::memory_order_relaxed);
  BlackboxRecord copy = record;
  copy.sequence = index + 1;
  records_[index % capacity_] = copy;
  header_->appended.store(index + 1, std::memory_order_release);
  if (index >= capacity_) {
    header_->dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

std::vector<BlackboxRecord> SharedBlackbox::Snapshot() const {
  std::vector<BlackboxRecord> snapshot;
  if (header_ == nullptr || records_ == nullptr || capacity_ == 0) {
    return snapshot;
  }
  const std::uint64_t appended =
      header_->appended.load(std::memory_order_acquire);
  const std::size_t count =
      std::min<std::size_t>(static_cast<std::size_t>(appended), capacity_);
  const std::size_t start =
      appended > capacity_ ? static_cast<std::size_t>(appended % capacity_)
                           : 0;
  snapshot.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    snapshot.push_back(records_[(start + i) % capacity_]);
  }
  return snapshot;
}

std::uint64_t SharedBlackbox::appended() const {
  return header_ == nullptr
             ? 0
             : header_->appended.load(std::memory_order_acquire);
}

std::uint64_t SharedBlackbox::dropped() const {
  return header_ == nullptr
             ? 0
             : header_->dropped.load(std::memory_order_relaxed);
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Build and run the Windows tests**

Run the Windows build block. Expected: `htmlppt_win32_tests` 14/14 (5 Job + 6 pipe + 3 blackbox); `htmlppt_core_tests` 189/189; Windows CTest 9/9. Run the Linux gate; expected 191/191, CTest 8/8.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/shared_blackbox.h \
  native/src/platform/win32/shared_blackbox.cpp \
  native/tests/integration/win32/shared_blackbox_test.cpp
git commit -m "feat: add win32 shared-memory black box"
```

---

### Task 4: Crash Dump, Windows Final Gate, and Documentation

**Files:**
- Create: `native/src/platform/win32/crash_dump.h`
- Create: `native/src/platform/win32/crash_dump.cpp`
- Create: `native/tests/integration/win32/crash_dump_test.cpp`
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-15-cpp-watchdog-win32-primitives.md` (as-built)

**Interfaces:**
- Consumes: Win32 `OpenProcess`, `CreateFileW`, `MiniDumpWriteDump`.
- Produces:
  - `bool WriteMiniDump(std::uint32_t pid, const std::filesystem::path& path);`

- [ ] **Step 1: Write the failing crash-dump tests**

Create `native/tests/integration/win32/crash_dump_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/win32/crash_dump.h"
#include "platform/win32/process_job.h"

using htmlppt::JobHandle;
using htmlppt::ProcessHandle;

namespace {

std::filesystem::path CmdExe() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"ComSpec", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return std::filesystem::path(buffer);
  return std::filesystem::path(L"C:\\Windows\\System32\\cmd.exe");
}

std::vector<std::wstring> SleepArgs() {
  return {L"/c", L"ping", L"-n", L"30", L"127.0.0.1"};
}

} // namespace

HTMLPPT_TEST_CASE(win32_crash_dump_writes_file) {
  JobHandle job;
  ProcessHandle process;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(process.LaunchSuspended(CmdExe(), SleepArgs(), job));
  HTMLPPT_CHECK(process.Resume());
  const auto path = std::filesystem::temp_directory_path() /
                    (L"htmlppt-dump-" + std::to_wstring(process.pid()) + L".dmp");
  std::error_code error;
  std::filesystem::remove(path, error);
  HTMLPPT_CHECK(htmlppt::WriteMiniDump(process.pid(), path));
  HTMLPPT_CHECK(std::filesystem::exists(path));
  HTMLPPT_CHECK(std::filesystem::file_size(path, error) > 0);
  HTMLPPT_CHECK(process.Terminate());
  HTMLPPT_CHECK(process.WaitForExit(5000));
  std::filesystem::remove(path, error);
}

HTMLPPT_TEST_CASE(win32_crash_dump_rejects_invalid_pid) {
  const auto path = std::filesystem::temp_directory_path() /
                    L"htmlppt-dump-invalid.dmp";
  std::error_code error;
  std::filesystem::remove(path, error);
  HTMLPPT_CHECK(!htmlppt::WriteMiniDump(0xFFFFFFFFu, path));
}

#endif // _WIN32
```

- [ ] **Step 2: Build and verify the tests fail**

Run the Windows build block. Expected: compilation fails because `platform/win32/crash_dump.h` does not exist.

- [ ] **Step 3: Add the crash-dump header and implementation**

Create `native/src/platform/win32/crash_dump.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>

namespace htmlppt {

bool WriteMiniDump(std::uint32_t pid, const std::filesystem::path& path);

} // namespace htmlppt
```

Create `native/src/platform/win32/crash_dump.cpp`:

```cpp
#include "platform/win32/crash_dump.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>

namespace htmlppt {

bool WriteMiniDump(std::uint32_t pid, const std::filesystem::path& path) {
  if (pid == 0 || path.empty()) return false;
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                               FALSE, pid);
  if (process == nullptr) return false;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    CloseHandle(process);
    return false;
  }
  const BOOL written = MiniDumpWriteDump(
      process, pid, file, MiniDumpNormal, nullptr, nullptr, nullptr);
  CloseHandle(file);
  CloseHandle(process);
  return written != FALSE;
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Document the Windows primitives and run the final gate**

Append to `native/README.md`:

````markdown
## Windows platform primitives (Plan 4B-1)

`platform/win32/process_job` creates a Job Object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, launches the worker suspended, assigns it
to the job, resumes it, terminates the job, and reports process liveness.

`platform/win32/named_pipe` implements an overlapped duplex pipe transport for
Plan 4A frames. `Receive` reads a 30-byte header, validates the payload length,
reads the payload, and decodes it with `DecodeFrame`.

`platform/win32/shared_blackbox` maps an 8 MB-capable file mapping with a
fixed header and `BlackboxRecord` array. `Append` is single-writer and
lock-free; `Snapshot` reads in chronological order and reports dropped
records.

`platform/win32/crash_dump` writes a MiniDump for a live process through
`MiniDumpWriteDump`.

Windows gate (MSVC 19.51 / Visual Studio 18 Build Tools):

```bash
WIN_SRC=/mnt/c/Temp/htmlplppt-win
rm -rf "$WIN_SRC" && mkdir -p "$WIN_SRC"
cp -r native "$WIN_SRC/native"
WIN_CMAKE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
WIN_CTEST="/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"
"$WIN_CMAKE" -S C:/Temp/htmlplppt-win/native -B C:/Temp/htmlplppt-win/build -G "Visual Studio 18 2026" -A x64 -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
"$WIN_CMAKE" --build C:/Temp/htmlplppt-win/build --config Release --parallel
"$WIN_CTEST" --test-dir C:/Temp/htmlplppt-win/build -C Release --output-on-failure
```

Expected: `htmlppt_core_tests` 189/189 (two pre-existing journal cases are
excluded on Windows), `htmlppt_win32_tests` 16/16, Windows CTest 9/9,
MSVC `/WX` clean.
````

Run the Linux gate:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
CHROME_BIN=/home/bingmoe/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome npm test
```

Expected: 191/191 unit cases, CTest 8/8, npm 3/3.

Windows gate: run the block above. Expected: `htmlppt_core_tests` 189/189
(two pre-existing journal cases are excluded on Windows), `htmlppt_win32_tests`
16/16, CTest 9/9, MSVC `/WX` clean.

- [ ] **Step 5: Record as-built amendments and commit**

Append an `As-Built Amendments (2026-11-15)` section to this plan with the actual Linux and Windows counts, any deviations, and deferred items: supervisor/worker host integration and CLI modes (Plan 4B-2), Edge Job handle duplication, shared-memory consumer wiring, 15-second legacy lock reclaim, and Windows end-to-end acceptance (Plan 8). Then:

```bash
git add native/src/platform/win32/crash_dump.h \
  native/src/platform/win32/crash_dump.cpp \
  native/tests/integration/win32/crash_dump_test.cpp native/README.md \
  docs/superpowers/plans/2026-11-15-cpp-watchdog-win32-primitives.md
git commit -m "feat: add win32 crash dump and platform gate"
```

---

## Self-Review

### Plan 4B-1 coverage

| Spec requirement | Plan 4B-1 coverage |
|---|---|
| Job Object, suspended worker launch, resume, terminate | Task 1 |
| Job close kills owned processes | Task 1 test |
| Named-pipe framed transport | Task 2 |
| Overlapped I/O and timeouts | Task 2 |
| Shared-memory black box | Task 3 |
| 96-byte record layout and chronological snapshot | Task 3 |
| MiniDump helper | Task 4 |
| Windows build and test on the real kernel | Every task's Windows gate |
| Linux build stays green | Every task's Linux gate |
| Platform-neutral core remains untouched | No core edits |

Deferred to Plan 4B-2: supervisor/worker hosts, CLI `--supervisor` / `--worker` / `--stop` / `--status` / `--dump-helper`, named-pipe frame routing, shared-memory consumer wiring, single-instance mutex creation, journal I/O, Edge Job handle duplication, and the Windows end-to-end smoke test. Deferred to Plan 5: WPS COM host, overlay, focus, Edge execution. Deferred to Plan 8: Windows acceptance and release.

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.

### Type consistency

- `JobHandle`/`ProcessHandle` are used by the Task 4 dump test and will be used by Plan 4B-2.
- `NamedPipeChannel` uses the Plan 4A `Frame`/`EncodeFrame`/`DecodeFrame`.
- `SharedBlackbox` uses the Plan 4A `BlackboxRecord` and preserves the 96-byte layout.
- `WriteMiniDump` is a free function used by the future `--dump-helper` mode.
- Windows test count: 5 Job + 6 pipe + 3 blackbox + 2 dump = 16.
- Windows CTest count: 8 existing + `win32_tests` = 9.

---

## Execution Handoff

Plan 4B-1 is ready. Execute Tasks 1-4 in order. Each task ends with both the Linux gate and the Windows gate green, plus a commit. Task 4 records the final gates and as-built amendments.

---

## As-Built Amendments (2026-11-15)

Plan 4B-1 is implemented and both final gates passed.

### Observed final-gate counts

Linux (GCC, `-Wall -Wextra -Wpedantic -Werror`):

- `htmlppt_core_tests`: 191/191 cases passed.
- CTest: 8/8 passed.
- `npm test`: 3/3 passed (`CHROME_BIN` pointed at the Playwright Chromium).

Windows (MSVC 19.51 / Visual Studio 18 Build Tools, Release x64):

- `htmlppt_core_tests`: 189/189 passed (the two pre-existing journal cases
  remain excluded on Windows).
- `htmlppt_win32_tests`: 16/16 passed: 5 Job, 6 named-pipe, 3 shared-blackbox,
  and 2 crash-dump cases.
- CTest: 9/9 passed.
- MSVC `/W4 /permissive- /utf-8 /WX` clean.

The final-review dispatch predicted 17/17 with "4 blackbox" cases, but the
suite contains three shared-blackbox cases (`round_trip`,
`wraps_and_counts_dropped`, `open_missing_returns_null`), so the observed
total is 16/16. The timing-sensitive named-pipe accept test was run twice and
passed both times.

TDD red was observed before implementation: the Windows build failed with
`crash_dump_test.cpp(11,10): error C1083: cannot open include file:
platform/win32/crash_dump.h`. Adding `crash_dump.h` and `crash_dump.cpp`
turned the build and both dump tests green.

### Deviations

- Spec 10.1 file layout: the implementation uses
  `platform/win32/named_pipe.*` plus `platform/win32/shared_blackbox.*`
  instead of a single `platform/win32/ipc.*` module. The split keeps the pipe
  transport and the shared-memory ring independently testable.
- The Task 2 brief was not applied verbatim: commit `b814ae6`
  (`fix: cancel overlapped pipe I/O on timeout`) added `CancelIoEx` plus a
  drain of the overlapped result on `Read`, `Write`, and `Accept` timeout, a
  fifth pipe timeout test, immediate `GetLastError()` capture in
  `NamedPipeClient::Connect`, and the timeout/`nullopt` contract comments.
- The final whole-branch review produced one fix wave of product and test
  changes (I1-I6 below), so the implementation is not a verbatim transcription
  of the plan blocks.
- Operational only: the sandbox rejected quoted executable paths containing
  spaces, so the Windows CMake/CTest binaries were invoked through the
  space-free symlinks in `/tmp/wintools/`. The commands and arguments are the
  ones documented in the README gate block.

### Final-review fixes (I1-I6)

1. I1 - `NamedPipeServer::Accept` now treats only `ERROR_IO_PENDING` as
   pending overlapped I/O; `ERROR_PIPE_CONNECTED` is success, and any other
   synchronous error closes the event and server and returns `nullptr`
   without waiting for the full timeout.
2. I2 - `ProcessHandle::LaunchSuspended` now assigns the process to the
   supplied `JobHandle` before returning and terminates it if the assignment
   fails; callers no longer call `JobHandle::Assign` separately.
3. I3 - `SharedBlackbox::Open` maps read-only and `Append` is a no-op on that
   mapping; the class comment records the single-writer serialization
   requirement and that `Snapshot` may return torn or out-of-order diagnostic
   records during a concurrent wrap.
4. I4 - The shared-black-box header has a `sizeof(Header) == 32`
   `static_assert` (inside the constructor, where the private nested type is
   accessible) and the magic is `0x424B4C42u` so the bytes read `BLKB` in
   little-endian memory.
5. I5 - `named_pipe.h` documents the `Accept` re-create requirement and the
   fixed 3-second partial-frame send contract.
6. I6 - New tests: `win32_named_pipe_accept_failure_does_not_wait`,
   `win32_process_resume_runs_child`, the pre-close `IsProcessAlive` check in
   `win32_job_kill_on_close`, and the read-only `Append` no-op check in
   `win32_shared_blackbox_round_trip`.

### Plan 4B-2 obligations

- Serialize shared-black-box writes through one writer thread or an MPSC
  queue. If multi-writer becomes required, replace the single-writer
  contract with a seqlock or an equivalent reader-consistent protocol.
- Validate the mapping size with `VirtualQuery` before taking live snapshots;
  `Open` currently trusts the header capacity.
- Map the supervisor's black-box view read-only; use `Create` only in the
  worker host.
- Define framed-transport state (`Timeout` / `Disconnected` / `BadFrame`) or
  keep the close-on-`nullopt` rule explicit in the supervisor loop.
- Escape embedded quotes and trailing backslashes in `CreateProcessW`
  command-line paths.
- Create pipe instances with `FIRST_PIPE_INSTANCE` and derive pipe names from
  a run id to avoid stale-instance and collision hazards.

### Deferred

- Plan 4B-2: supervisor/worker host integration and CLI modes
  (`--supervisor`, `--worker`, `--stop`, `--status`, `--dump-helper`),
  named-pipe frame routing, shared-memory consumer wiring, single-instance
  mutex creation, journal I/O, Edge Job handle duplication, and the Windows
  end-to-end smoke test.
- Plan 4B-2/5: 15-second legacy lock reclaim.
- Plan 5: WPS COM host, overlay, focus, and Edge execution.
- Plan 8: Windows end-to-end acceptance and release.
