# C++ Watchdog Supervisor Host Implementation Plan (Plan 4B-2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the Plan 4A supervisor core and Plan 4B-1 Windows primitives into a working supervisor/worker host, add the `--supervisor` / `--worker` / `--stop` / `--status` / `--dump-helper` CLI modes, enforce single-instance, and verify the host on the real Windows kernel.

**Architecture:** `app/supervisor` and `app/worker` are the Windows host layer. The supervisor owns the Job Object, named-pipe server, shared black box, stop event, journal, and restart loop; it feeds frames into `SupervisorCore` and executes its declarative actions. The worker opens the pipe and black box, sends 200 ms heartbeats, and exits on a Stop frame. Linux builds keep the existing diagnostic CLI and return a clear Windows-only error for the host modes.

**Tech Stack:** C++20, Win32, MSVC 19.51 via Visual Studio 18 Build Tools, Plan 4A `SupervisorCore`, Plan 4B-1 `JobHandle`/`ProcessHandle`/`NamedPipe*`/`SharedBlackbox`/`WriteMiniDump`, Plan 1 `JsonlJournal`, and the existing test harness.

## Global Constraints

- Target Windows 10/11 x64; the Linux/WSL diagnostic build must stay green.
- C++20; MSVC `/W4 /permissive- /utf-8 /WX`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only.
- `core/` stays platform-neutral. Windows headers and host code live in `src/app/` under `_WIN32` or in `src/platform/win32/`.
- Supervisor defaults: heartbeat 200 ms from the worker, 2 s heartbeat timeout, 5 restarts per 10 minutes, backoff 200/500/1000/2000/5000 ms.
- The shared black box is single-writer: the worker is the only writer; the supervisor reads a read-only view.
- Named-pipe server accept failure/timeout closes the server; the host must recreate it before accepting again.
- Existing Linux gate: 191/191 unit cases, CTest 8/8, `npm test` 3/3 with `CHROME_BIN`.
- Windows gate after this plan: `htmlppt_core_tests` 189/189, `htmlppt_win32_tests` 21/21, CTest 9/9, `/WX` clean.

## Windows Build Toolchain

```text
CMake/CTest: C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\
Generator: Visual Studio 18 2026
MSVC: 19.51.36252.0 (14.51.36231)
Windows SDK: 10.0.26100.0
```

Use space-free symlinks if the sandbox rejects the quoted tool paths:

```bash
mkdir -p /tmp/wintools
ln -sf "/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" /tmp/wintools/cmake
ln -sf "/mnt/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" /tmp/wintools/ctest
```

Windows build/test loop (from the worktree root):

```bash
WIN_SRC=/mnt/c/Temp/htmlplppt-win
rm -rf "$WIN_SRC" && mkdir -p "$WIN_SRC"
cp -r native "$WIN_SRC/native"
/tmp/wintools/cmake -S C:/Temp/htmlplppt-win/native -B C:/Temp/htmlplppt-win/build \
  -G "Visual Studio 18 2026" -A x64 -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
/tmp/wintools/cmake --build C:/Temp/htmlplppt-win/build --config Release --parallel
/tmp/wintools/ctest --test-dir C:/Temp/htmlplppt-win/build -C Release --output-on-failure
```

## File Structure

Create:

- `native/src/app/supervisor.h` - host options, run/stop/status/dump entry points, name helpers.
- `native/src/app/supervisor.cpp` - supervisor loop, worker loop, stop/status/dump helper, Linux stubs.
- `native/tests/integration/win32/supervisor_host_test.cpp` - supervisor/worker and CLI integration tests.

Modify:

- `native/src/platform/win32/shared_blackbox.h/.cpp` - add `OpenWritable` for the worker.
- `native/src/app/cli.h/.cpp` - add host commands and `--run-id`.
- `native/CMakeLists.txt` - add `app/supervisor.cpp`, link `htmlppt_win32` on WIN32, add the host test source and `HTMLPPT_EXE_PATH`.
- `native/README.md` - document the host modes and Windows acceptance.
- `docs/superpowers/plans/2026-11-15-cpp-watchdog-supervisor-host.md` - as-built amendments.

---

### Task 1: Supervisor/Worker Host and Start/Stop CLI

**Files:**
- Create: `native/src/app/supervisor.h`
- Create: `native/src/app/supervisor.cpp`
- Modify: `native/src/platform/win32/shared_blackbox.h`
- Modify: `native/src/platform/win32/shared_blackbox.cpp`
- Modify: `native/src/app/cli.cpp`
- Modify: `native/CMakeLists.txt`
- Create: `native/tests/integration/win32/supervisor_host_test.cpp`

**Interfaces:**
- Consumes: `SupervisorCore`, `SupervisorConfig`, `WorkerEvent`, `WorkerEventKind`; `JobHandle`, `ProcessHandle`, `NamedPipeServer`, `NamedPipeChannel`, `NamedPipeClient`, `Frame`, `FrameType`, `SharedBlackbox`, `WriteMiniDump`, `JsonlJournal`, `MakeInstanceIdentity`.
- Produces:
  - `struct SupervisorRunOptions { std::filesystem::path state_dir; std::string run_id; }`
  - `int RunSupervisor(const SupervisorRunOptions&)`
  - `int RunWorker(const SupervisorRunOptions&)`
  - `int RunDumpHelper(std::uint32_t pid, const std::filesystem::path& path)`
  - Name helpers: `SupervisorPipeName(run_id)`, `SupervisorBlackboxName(run_id)`, `SupervisorStopEventName(run_id)` returning `std::wstring`
  - `SharedBlackbox::OpenWritable(const std::wstring& name)`

- [ ] **Step 1: Add the host header**

Create `native/src/app/supervisor.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace htmlppt {

struct SupervisorRunOptions {
  std::filesystem::path state_dir;
  std::string run_id;
};

inline std::wstring SupervisorPipeName(std::string_view run_id) {
  return L"\\\\.\\pipe\\htmlppt-" +
         std::wstring(run_id.begin(), run_id.end());
}

inline std::wstring SupervisorBlackboxName(std::string_view run_id) {
  return L"Local\\htmlppt-blackbox-" +
         std::wstring(run_id.begin(), run_id.end());
}

inline std::wstring SupervisorStopEventName(std::string_view run_id) {
  return L"Local\\htmlppt-stop-" +
         std::wstring(run_id.begin(), run_id.end());
}

int RunSupervisor(const SupervisorRunOptions& options);
int RunWorker(const SupervisorRunOptions& options);
int RunStop(std::string_view run_id);
int RunStatus(std::string_view run_id,
              const std::filesystem::path& state_dir);
int RunDumpHelper(std::uint32_t pid, const std::filesystem::path& path);

} // namespace htmlppt
```

- [ ] **Step 2: Add `OpenWritable` to the shared black box**

In `native/src/platform/win32/shared_blackbox.h`, add:

```cpp
  static std::unique_ptr<SharedBlackbox> OpenWritable(const std::wstring& name);
```

In `native/src/platform/win32/shared_blackbox.cpp`, add a private helper and the new method. Replace the current `Open` body with a shared `OpenWithAccess` helper:

```cpp
std::unique_ptr<SharedBlackbox> SharedBlackbox::Open(const std::wstring& name) {
  return OpenWithAccess(name, FILE_MAP_READ, false);
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::OpenWritable(
    const std::wstring& name) {
  return OpenWithAccess(name, FILE_MAP_ALL_ACCESS, true);
}
```

Add the private member function declaration `static std::unique_ptr<SharedBlackbox> OpenWithAccess(const std::wstring&, DWORD, bool);` and define it with the current `Open` body, changing the `MapViewOfFile` access argument and the constructor `writable` flag.

- [ ] **Step 3: Add the supervisor/worker implementation**

Create `native/src/app/supervisor.cpp`:

```cpp
#include "app/supervisor.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>

#include "core/journal.h"
#include "core/single_instance.h"
#include "core/supervisor.h"
#include "platform/win32/crash_dump.h"
#include "platform/win32/named_pipe.h"
#include "platform/win32/process_job.h"
#include "platform/win32/shared_blackbox.h"

namespace htmlppt {
namespace {

std::wstring Widen(const std::string& text) {
  return std::wstring(text.begin(), text.end());
}

std::string Narrow(const std::wstring& text) {
  return std::string(text.begin(), text.end());
}

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::filesystem::path DefaultStateDir() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length =
      GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    return std::filesystem::path(buffer) / L"htmlppt-watchdog";
  }
  return std::filesystem::temp_directory_path() / L"htmlppt-watchdog";
}

std::string NewRunId() {
  return "run-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64());
}

std::filesystem::path CurrentExecutable() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return std::filesystem::path(std::wstring(buffer, length));
}

std::wstring UserStateDirOption(const std::filesystem::path& path) {
  return path.wstring();
}

JournalRecord MakeRecord(std::string run_id, std::string state,
                         std::string event, std::string detail) {
  JournalRecord record;
  record.run_id = std::move(run_id);
  record.state = std::move(state);
  record.event = std::move(event);
  record.detail = std::move(detail);
  return record;
}

void AppendJournal(Journal& journal, const std::string& run_id,
                   const std::string& state, const std::string& event,
                   const std::string& detail) {
  journal.append(MakeRecord(run_id, state, event, detail));
  journal.flush();
}

WorkerEvent MakeEvent(WorkerEventKind kind, std::uint32_t sequence = 0) {
  WorkerEvent event;
  event.kind = kind;
  event.at = SteadyClock{}.now();
  event.sequence = sequence;
  return event;
}

} // namespace

int RunDumpHelper(std::uint32_t pid, const std::filesystem::path& path) {
  return WriteMiniDump(pid, path) ? 0 : 1;
}

int RunStop(std::string_view run_id) {
  if (run_id.empty()) return 2;
  const std::wstring name = SupervisorStopEventName(run_id);
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
  if (event == nullptr) {
    std::cerr << "error: no running supervisor for run " << run_id << "\n";
    return 1;
  }
  SetEvent(event);
  CloseHandle(event);
  return 0;
}

int RunStatus(std::string_view run_id,
              const std::filesystem::path& state_dir) {
  if (run_id.empty()) return 2;
  auto blackbox = SharedBlackbox::Open(SupervisorBlackboxName(run_id));
  if (blackbox == nullptr) {
    std::cout << "no blackbox for run " << run_id << "\n";
    return 1;
  }
  const auto snapshot = blackbox->Snapshot();
  std::cout << "run " << run_id << " records " << snapshot.size()
            << " appended " << blackbox->appended() << " dropped "
            << blackbox->dropped() << "\n";
  std::size_t start = 0;
  if (snapshot.size() > 10) start = snapshot.size() - 10;
  for (std::size_t i = start; i < snapshot.size(); ++i) {
    const auto& record = snapshot[i];
    std::cout << record.sequence << " " << record.timestamp_ms << " "
              << static_cast<int>(record.event) << " " << record.state
              << " " << record.detail << "\n";
  }
  const auto journal_path = state_dir / "journal.jsonl";
  std::cout << "journal " << journal_path.string() << "\n";
  return 0;
}

int RunSupervisor(const SupervisorRunOptions& options) {
  const auto state_dir = options.state_dir.empty() ? DefaultStateDir()
                                                   : options.state_dir;
  std::error_code error;
  std::filesystem::create_directories(state_dir, error);
  const std::string run_id =
      options.run_id.empty() ? NewRunId() : options.run_id;

  const std::wstring stop_name = SupervisorStopEventName(run_id);
  HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str());
  if (stop_event == nullptr) {
    std::cerr << "error: cannot create stop event\n";
    return 1;
  }
  ResetEvent(stop_event);

  const auto identity = MakeInstanceIdentity(state_dir, "supervisor");
  const std::wstring mutex_name(identity.mutex_name.begin(),
                                identity.mutex_name.end());
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
  if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
    std::cerr << "error: another supervisor is running for this state dir\n";
    if (mutex != nullptr) CloseHandle(mutex);
    CloseHandle(stop_event);
    return 1;
  }

  auto blackbox = SharedBlackbox::Create(SupervisorBlackboxName(run_id), 8192);
  if (blackbox == nullptr) {
    std::cerr << "error: cannot create blackbox mapping\n";
    CloseHandle(mutex);
    CloseHandle(stop_event);
    return 1;
  }

  JsonlJournal journal(state_dir / "journal.jsonl");
  AppendJournal(journal, run_id, "Supervisor", "start", "supervisor start");

  NamedPipeServer server;
  if (!server.Create(SupervisorPipeName(run_id))) {
    std::cerr << "error: cannot create named pipe\n";
    CloseHandle(mutex);
    CloseHandle(stop_event);
    return 1;
  }

  JobHandle job;
  if (!job.Create()) {
    std::cerr << "error: cannot create job object\n";
    CloseHandle(mutex);
    CloseHandle(stop_event);
    return 1;
  }

  SteadyClock clock;
  SupervisorConfig core_config;
  SupervisorCore core(core_config, clock);
  core.Start();

  ProcessHandle worker;
  const auto exe = CurrentExecutable();
  const auto launch_worker = [&]() -> bool {
    std::vector<std::wstring> args = {
        L"--worker", L"--run-id", Widen(run_id), L"--state-dir",
        state_dir.wstring()};
    return worker.LaunchSuspended(exe, args, job) && worker.Resume();
  };

  std::unique_ptr<NamedPipeChannel> channel;
  const auto cleanup = [&]() {
    if (channel != nullptr) channel->Close();
    server.Close();
    job.Terminate();
    AppendJournal(journal, run_id, "Exited", "stop", "supervisor stop");
    blackbox.reset();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CloseHandle(stop_event);
  };

  if (!launch_worker()) {
    cleanup();
    return 1;
  }

  const auto create_dump = [&](std::uint32_t pid) {
    const auto crash_dir = state_dir / "crash";
    std::filesystem::create_directories(crash_dir, error);
    const auto path = crash_dir / (std::to_wstring(pid) + L".dmp");
    const std::wstring command = L"\"" + exe.wstring() + L"\" --dump-helper " +
                                 std::to_wstring(pid) + L" \"" +
                                 path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
    if (information.hProcess != nullptr) CloseHandle(information.hProcess);
    if (information.hThread != nullptr) CloseHandle(information.hThread);
  };

  const auto apply = [&](const std::vector<SupervisorAction>& actions)
      -> std::optional<int> {
    for (const auto& action : actions) {
      switch (action.kind) {
        case SupervisorActionKind::StartWorker:
          break;
        case SupervisorActionKind::RequestDump:
          if (worker.pid() != 0) create_dump(worker.pid());
          break;
        case SupervisorActionKind::TerminateWorker:
          job.Terminate();
          if (channel != nullptr) {
            channel->Close();
            channel.reset();
          }
          break;
        case SupervisorActionKind::RestartWorker:
          channel.reset();
          server.Close();
          server.Create(SupervisorPipeName(run_id));
          if (!launch_worker()) return 1;
          break;
        case SupervisorActionKind::EnterSafeMode:
          AppendJournal(journal, run_id, "SafeMode", "action", action.detail);
          cleanup();
          return 3;
        case SupervisorActionKind::RunLegacyFallback:
          AppendJournal(journal, run_id, "SafeMode", "legacy", action.detail);
          cleanup();
          return 4;
        case SupervisorActionKind::StopComplete:
          cleanup();
          return 0;
      }
    }
    return std::nullopt;
  };

  for (;;) {
    if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) {
      if (channel != nullptr) {
        Frame frame;
        frame.header.type = FrameType::Stop;
        frame.header.sequence = 1;
        frame.header.timestamp_ms = static_cast<std::uint64_t>(NowMs());
        channel->Send(frame);
        worker.WaitForExit(2000);
      }
      cleanup();
      return 0;
    }

    if (channel == nullptr) {
      channel = server.Accept(Duration{500});
      if (channel == nullptr) {
        if (worker.WaitForExit(0)) {
          if (auto code = apply(core.Handle(MakeEvent(WorkerEventKind::Exited)));
              code.has_value()) {
            return *code;
          }
        } else if (auto code = apply(core.Tick()); code.has_value()) {
          return *code;
        }
        continue;
      }
    }

    const auto frame = channel->Receive(Duration{100});
    if (frame.has_value()) {
      switch (frame->header.type) {
        case FrameType::Heartbeat:
          if (auto code = apply(core.Handle(
                  MakeEvent(WorkerEventKind::Heartbeat,
                            frame->header.sequence)));
              code.has_value()) {
            return *code;
          }
          break;
        case FrameType::Stop:
          if (auto code =
                  apply(core.Handle(MakeEvent(WorkerEventKind::StopRequested)));
              code.has_value()) {
            return *code;
          }
          break;
        case FrameType::Cleanup:
          if (auto code =
                  apply(core.Handle(MakeEvent(WorkerEventKind::CleanupDone)));
              code.has_value()) {
            return *code;
          }
          break;
        default:
          break;
      }
    } else if (worker.WaitForExit(0)) {
      if (auto code = apply(core.Handle(MakeEvent(WorkerEventKind::Exited)));
          code.has_value()) {
        return *code;
      }
    } else if (auto code = apply(core.Tick()); code.has_value()) {
      return *code;
    }
  }
}

int RunWorker(const SupervisorRunOptions& options) {
  const auto state_dir = options.state_dir.empty() ? DefaultStateDir()
                                                   : options.state_dir;
  const std::string run_id =
      options.run_id.empty() ? NewRunId() : options.run_id;
  auto blackbox = SharedBlackbox::OpenWritable(SupervisorBlackboxName(run_id));
  if (blackbox == nullptr) {
    std::cerr << "error: cannot open blackbox\n";
    return 1;
  }
  NamedPipeClient client;
  if (!client.Connect(SupervisorPipeName(run_id), Duration{5000})) {
    std::cerr << "error: cannot connect to supervisor pipe\n";
    return 1;
  }
  std::uint32_t sequence = 0;
  for (;;) {
    const auto command = client.Receive(Duration{10});
    if (command.has_value() && command->header.type == FrameType::Stop) {
      Frame cleanup;
      cleanup.header.type = FrameType::Cleanup;
      cleanup.header.sequence = ++sequence;
      cleanup.header.timestamp_ms = static_cast<std::uint64_t>(NowMs());
      client.Send(cleanup);
      return 0;
    }
    Frame heartbeat;
    heartbeat.header.type = FrameType::Heartbeat;
    heartbeat.header.sequence = ++sequence;
    heartbeat.header.timestamp_ms = static_cast<std::uint64_t>(NowMs());
    if (!client.Send(heartbeat)) return 1;
    if ((sequence % 5) == 0) {
      BlackboxRecord record;
      record.timestamp_ms = NowMs();
      record.event = BlackboxEvent::Heartbeat;
      record.state = 0;
      record.value = sequence;
      std::snprintf(record.detail, sizeof(record.detail), "worker heartbeat");
      blackbox->Append(record);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

} // namespace htmlppt

#else  // !_WIN32

namespace htmlppt {

int RunSupervisor(const SupervisorRunOptions&) {
  std::cerr << "error: --supervisor requires Windows\n";
  return 2;
}

int RunWorker(const SupervisorRunOptions&) {
  std::cerr << "error: --worker requires Windows\n";
  return 2;
}

int RunStop(std::string_view) {
  std::cerr << "error: --stop requires Windows\n";
  return 2;
}

int RunStatus(std::string_view, const std::filesystem::path&) {
  std::cerr << "error: --status requires Windows\n";
  return 2;
}

int RunDumpHelper(std::uint32_t, const std::filesystem::path&) {
  std::cerr << "error: --dump-helper requires Windows\n";
  return 2;
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Add the CLI host modes**

In `native/src/app/cli.cpp`:
- Include `app/supervisor.h`.
- Extend `Command` with `Supervisor`, `Worker`, `Stop`, `Status`, `DumpHelper`.
- Add `std::string run_id;` and `std::uint32_t dump_pid = 0; std::string dump_path;` to `CliOptions`.
- Add `--supervisor`, `--worker`, `--stop`, `--status`, `--dump-helper PID PATH`, `--run-id ID`.
- `--dump-helper` uses `TakeValue` twice; parse the pid with `std::from_chars` (>=1).
- Command rules:
  - `--supervisor`/`--worker` require no config.
  - `--stop`/`--status` require `--run-id`.
  - `--dump-helper` requires pid/path.
  - `--run-id` is only valid with host commands.
- Dispatch:
  - `Supervisor`: `RunSupervisor({state_dir, run_id})`.
  - `Worker`: `RunWorker({state_dir, run_id})`.
  - `Stop`: `RunStop(run_id)`.
  - `Status`: `RunStatus(run_id, state_dir)`.
  - `DumpHelper`: `RunDumpHelper(dump_pid, dump_path)`.
  - `state_dir` is `options.overrides.state_dir.value_or("")`.
- Print usage with the new commands.
- Keep all existing diagnostics unchanged.

- [ ] **Step 5: CMake and the start/stop integration test**

In `native/CMakeLists.txt`:
- Add `src/app/supervisor.cpp` to `htmlppt-watchdog`.
- On WIN32, link `htmlppt_win32` to `htmlppt-watchdog`.
- Add the host test source to `htmlppt_win32_tests` and define the EXE path:

```cmake
    target_compile_definitions(htmlppt_win32_tests PRIVATE
      "HTMLPPT_EXE_PATH=\"$<TARGET_FILE:htmlppt-watchdog>\"")
    add_dependencies(htmlppt_win32_tests htmlppt-watchdog)
```

Create `native/tests/integration/win32/supervisor_host_test.cpp` with a start/stop test:

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app/supervisor.h"
#include "platform/win32/process_job.h"
#include "platform/win32/shared_blackbox.h"

using htmlppt::JobHandle;
using htmlppt::ProcessHandle;
using htmlppt::SharedBlackbox;

namespace {

std::filesystem::path ExePath() {
  return std::filesystem::u8path(HTMLPPT_EXE_PATH);
}

std::filesystem::path TempStateDir() {
  const auto path = std::filesystem::temp_directory_path() /
                    (L"htmlppt-host-test-" +
                     std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(path);
  return path;
}

std::string RunId() {
  return "test-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64());
}

HANDLE WaitForStopEvent(const std::wstring& name, int timeout_ms) {
  const auto deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (event != nullptr) return event;
    if (GetTickCount64() >= deadline) return nullptr;
    Sleep(100);
  }
}

} // namespace

HTMLPPT_TEST_CASE(win32_supervisor_starts_and_stops) {
  const auto state_dir = TempStateDir();
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());

  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  bool saw_records = false;
  for (int i = 0; i < 30; ++i) {
    auto blackbox = SharedBlackbox::Open(
        htmlppt::SupervisorBlackboxName(run_id));
    if (blackbox != nullptr && blackbox->appended() > 0) {
      saw_records = true;
      break;
    }
    Sleep(100);
  }
  HTMLPPT_CHECK(saw_records);

  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  DWORD exit_code = 0;
  GetExitCodeProcess(supervisor.process(), &exit_code);
  HTMLPPT_CHECK_EQ(exit_code, DWORD{0});
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

#endif // _WIN32
```

- [ ] **Step 6: Build and run both gates**

Linux gate:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: 191/191 unit, CTest 8/8.

Windows gate: run the Windows loop. Expected: core 189/189, win32 17/17 (16 + 1 host test), CTest 9/9, `/WX` clean. Run the win32 suite twice for the start/stop timing.

- [ ] **Step 7: Commit**

```bash
git add native/src/app/supervisor.h native/src/app/supervisor.cpp \
  native/src/platform/win32/shared_blackbox.h \
  native/src/platform/win32/shared_blackbox.cpp \
  native/src/app/cli.cpp native/CMakeLists.txt \
  native/tests/integration/win32/supervisor_host_test.cpp
git commit -m "feat: add supervisor and worker host"
```

---

### Task 2: Stop, Status, Dump Helper, and Single-Instance Tests

**Files:**
- Modify: `native/tests/integration/win32/supervisor_host_test.cpp`
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-15-cpp-watchdog-supervisor-host.md` (as-built)

**Interfaces:**
- Consumes: Task 1 host entry points and name helpers.
- Produces: integration coverage for `--stop`, `--status`, `--dump-helper`, and single-instance.

- [ ] **Step 1: Add the CLI/integration tests**

Extend `native/tests/integration/win32/supervisor_host_test.cpp` with a `RunCli` helper and three tests:

```cpp
namespace {

struct CliResult {
  bool completed = false;
  DWORD exit_code = 0;
};

CliResult RunCli(const std::vector<std::wstring>& args,
                 DWORD timeout_ms = 10000) {
  JobHandle job;
  ProcessHandle process;
  if (!job.Create() || !process.LaunchSuspended(ExePath(), args, job) ||
      !process.Resume()) {
    return {};
  }
  if (!process.WaitForExit(timeout_ms)) {
    process.Terminate();
    process.WaitForExit(2000);
    return {};
  }
  CliResult result;
  result.completed = true;
  GetExitCodeProcess(process.process(), &result.exit_code);
  return result;
}

} // namespace
```

Add the tests:

```cpp
HTMLPPT_TEST_CASE(win32_supervisor_single_instance) {
  const auto state_dir = TempStateDir();
  const std::string first_id = RunId();
  const std::string second_id = RunId() + "-second";
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(first_id.begin(), first_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(first_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);

  const auto duplicate = RunCli(
      {L"--supervisor", L"--state-dir", state_dir.wstring(), L"--run-id",
       std::wstring(second_id.begin(), second_id.end())},
      5000);
  HTMLPPT_CHECK(duplicate.completed);
  HTMLPPT_CHECK_EQ(duplicate.exit_code, DWORD{1});

  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_stop) {
  const auto state_dir = TempStateDir();
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);
  CloseHandle(stop);

  const auto stop_result = RunCli(
      {L"--stop", L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      5000);
  HTMLPPT_CHECK(stop_result.completed);
  HTMLPPT_CHECK_EQ(stop_result.exit_code, DWORD{0});
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_status) {
  const auto state_dir = TempStateDir();
  const std::string run_id = RunId();
  JobHandle job;
  ProcessHandle supervisor;
  HTMLPPT_CHECK(job.Create());
  HTMLPPT_CHECK(supervisor.LaunchSuspended(
      ExePath(), {L"--supervisor", L"--state-dir", state_dir.wstring(),
                  L"--run-id", std::wstring(run_id.begin(), run_id.end())},
      job));
  HTMLPPT_CHECK(supervisor.Resume());
  HANDLE stop = WaitForStopEvent(
      htmlppt::SupervisorStopEventName(run_id), 5000);
  HTMLPPT_CHECK(stop != nullptr);
  Sleep(1500);
  const auto status = RunCli(
      {L"--status", L"--run-id", std::wstring(run_id.begin(), run_id.end()),
       L"--state-dir", state_dir.wstring()},
      5000);
  HTMLPPT_CHECK(status.completed);
  HTMLPPT_CHECK_EQ(status.exit_code, DWORD{0});
  SetEvent(stop);
  CloseHandle(stop);
  HTMLPPT_CHECK(supervisor.WaitForExit(10000));
  std::error_code error;
  std::filesystem::remove_all(state_dir, error);
}

HTMLPPT_TEST_CASE(win32_cli_dump_helper) {
  JobHandle child_job;
  ProcessHandle child;
  HTMLPPT_CHECK(child_job.Create());
  HTMLPPT_CHECK(child.LaunchSuspended(
      L"C:\\Windows\\System32\\cmd.exe",
      {L"/c", L"ping", L"-n", L"30", L"127.0.0.1"}, child_job));
  HTMLPPT_CHECK(child.Resume());
  const auto path = std::filesystem::temp_directory_path() /
                    (L"htmlppt-cli-dump-" + std::to_wstring(child.pid()) +
                     L".dmp");
  std::error_code error;
  std::filesystem::remove(path, error);
  const auto dump = RunCli(
      {L"--dump-helper", std::to_wstring(child.pid()), path.wstring()},
      5000);
  HTMLPPT_CHECK(dump.completed);
  HTMLPPT_CHECK_EQ(dump.exit_code, DWORD{0});
  HTMLPPT_CHECK(std::filesystem::exists(path));
  HTMLPPT_CHECK(std::filesystem::file_size(path, error) > 0);
  HTMLPPT_CHECK(!error);
  HTMLPPT_CHECK(child.Terminate());
  HTMLPPT_CHECK(child.WaitForExit(5000));
  std::filesystem::remove(path, error);
}
```

- [ ] **Step 2: Build and run both gates**

Linux: 191/191, CTest 8/8, npm 3/3.
Windows: core 189/189, win32 21/21 (16 + 4 host/CLI tests), CTest 9/9, `/WX` clean. Run the win32 suite twice.

- [ ] **Step 3: Document and record as-built**

Append to `native/README.md`:

````markdown
## Supervisor and worker host (Plan 4B-2)

The same EXE runs as supervisor and worker.

- `--supervisor --state-dir <dir> --run-id <id>` creates the stop event, single-instance mutex, shared black box, named-pipe server, and worker Job; it monitors 200 ms heartbeats, restarts the worker with the Plan 4A budget/backoff, and exits after `--stop`.
- `--worker --run-id <id>` connects to the pipe, sends a heartbeat every 200 ms, appends a black-box record every five heartbeats, and exits on a Stop frame.
- `--stop --run-id <id>` sets the named stop event.
- `--status --run-id <id> --state-dir <dir>` prints the last black-box records and the journal path.
- `--dump-helper <pid> <path>` writes a MiniDump.

The shared black box is written only by the worker; the supervisor reads a read-only view. On Linux these host modes print a Windows-only error and exit 2; the diagnostic CLI is unchanged.

Windows gate: `htmlppt_core_tests` 189/189, `htmlppt_win32_tests` 21/21, CTest 9/9, `/WX` clean.
````

Append an `As-Built Amendments (2026-11-15)` section to this plan with the actual Linux/Windows counts, deviations, and deferred items (Edge Job handle duplication and WPS/overlay/focus integration to Plan 5; full Windows acceptance to Plan 8; status/journal formatting extensions later).

- [ ] **Step 4: Commit**

```bash
git add native/tests/integration/win32/supervisor_host_test.cpp \
  native/README.md docs/superpowers/plans/2026-11-15-cpp-watchdog-supervisor-host.md
git commit -m "docs: record Plan 4B-2 as-built amendments"
```

---

## Self-Review

### Plan 4B-2 coverage

| Spec requirement | Plan 4B-2 coverage |
|---|---|
| Single EXE `--supervisor`/`--worker` roles | Task 1 |
| Job Object owns worker; suspended launch/assign/resume | Task 1 uses Plan 4B-1 primitives |
| Named-pipe heartbeat/status frames | Task 1 worker/supervisor |
| 200 ms heartbeats, 2 s timeout, restart budget/backoff | Task 1 + Plan 4A core |
| Shared black box read by supervisor, written by worker | Task 1 + `OpenWritable` |
| Stop event, `--stop` | Task 1/2 |
| `--status`, journal path | Task 2 |
| `--dump-helper` MiniDump | Task 1/2 |
| Single-instance mutex | Task 1/2 |
| Linux diagnostic CLI unchanged | Task 1 stubs |
| Windows integration tests | Tasks 1/2 |

Deferred to Plan 5: WPS COM host, overlay, focus, Edge execution, Edge Job handle duplication, legacy flags/launcher. Deferred to Plan 6: scheduling/priorities/power/fault injection. Deferred to Plan 8: full Windows end-to-end acceptance, diagnostics packaging, release.

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.

### Type consistency

- `SupervisorRunOptions` is shared by supervisor, worker, and CLI.
- `SupervisorCore` actions are consumed by `RunSupervisor::apply`.
- `SharedBlackbox::OpenWritable` is used only by the worker; `Open` remains read-only.
- The name helpers are inline and used by host and tests.
- Windows test count: 16 existing + 1 start/stop + 4 Task 2 = 21? The plan's Task 1 says 17 (16+1); Task 2 says 20 (16+4). Wait: Task 1 adds 1 -> 17. Task 2 adds 4 -> 21 (17+4). The plan text says 20 in Step 2 and README. Correct it to 21. Recompute: 4B-1 final win32 16. Task 1 adds `win32_supervisor_starts_and_stops` = 17. Task 2 adds single_instance, cli_stop, cli_status, cli_dump_helper = 21. So Windows win32 21/21. The plan's Task 2 Step 2 says 21/21 and README says 21/21; update to 21/21. The self-review table no count. Also the Global Constraints says 21/21; update to 21/21.
- CTest remains 9.

---

## Execution Handoff

Plan 4B-2 is ready. Execute Tasks 1-2 in order. Each task ends with both gates green and a commit. Task 2 records the final gates and as-built.
