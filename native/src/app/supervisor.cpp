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
        // Accept failure or timeout closes the server handle; restore a
        // listening instance so a still-connecting worker can attach.
        server.Create(SupervisorPipeName(run_id));
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
