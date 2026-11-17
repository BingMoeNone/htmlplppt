#include "app/supervisor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include "app/worker_session.h"
#include "core/blackbox.h"
#include "core/compat_flags.h"
#include "core/config.h"
#include "core/config_issue.h"
#include "core/deck_info.h"
#include "core/journal.h"
#include "core/resolve.h"
#include "core/single_instance.h"
#include "core/supervisor.h"
#include "core/utf8_path.h"
#include "core/worker_plan.h"
#include "core/worker_runtime.h"
#include "platform/win32/crash_dump.h"
#include "platform/win32/cover_overlay_win32.h"
#include "platform/win32/edge_launcher_win32.h"
#include "platform/win32/named_pipe.h"
#include "platform/win32/process_job.h"
#include "platform/win32/process_query.h"
#include "platform/win32/shared_blackbox.h"
#include "platform/win32/window_system_win32.h"
#include "platform/win32/wps_host.h"

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

// Names the first error of a failed config/deck/resolve step as one readable
// line: "path: message" or just the message when the issue has no path.
std::string FirstError(const std::vector<ConfigIssue>& issues) {
  for (const auto& issue : issues) {
    if (issue.severity != IssueSeverity::Error) continue;
    if (issue.path.empty()) return issue.message;
    return issue.path + ": " + issue.message;
  }
  return "unknown error";
}
// The WPS processes that can own a slideshow window. Zero pids is a defect:
// focus and return would have no WPS window to target, so the caller reports it.
//
// Process identity is the reliable source: the slideshow window class differs
// between builds (Office uses screenClass/SlideShowClass; WPS 12's show window
// exposes neither, and its PP12FrameClass belongs to the editor frame), so a
// class whitelist silently returns zero on WPS. SelectWpsShowWindow still scores
// the windows of these pids, which keeps the editor out of the pick.
std::vector<std::uint32_t> CollectWpsPids() {
  std::vector<std::uint32_t> pids;
  for (const wchar_t* name : {L"wpp.exe", L"kwpp.exe", L"wps.exe"}) {
    for (const auto id : ProcessIdsByName(name)) {
      if (std::find(pids.begin(), pids.end(), id) == pids.end()) {
        pids.push_back(id);
      }
    }
  }
  return pids;
}

void AppendWorkerDiagnostic(SharedBlackbox& blackbox, BlackboxEvent event,
                            std::int64_t value, const std::string& detail) {
  BlackboxRecord record;
  record.timestamp_ms = NowMs();
  record.event = event;
  record.pid = GetCurrentProcessId();
  record.value = value;
  std::snprintf(record.detail, sizeof(record.detail), "%s", detail.c_str());
  blackbox.Append(record);
}

// Hides the cover and releases the COM host on every RunWorker exit path,
// including the early returns taken after the runtime has run.
class WorkerHostGuard {
 public:
  WorkerHostGuard(PresentationHost& presentation, CoverOverlay& overlay)
      : presentation_(presentation), overlay_(overlay) {}
  ~WorkerHostGuard() {
    overlay_.Hide();
    presentation_.Release();
  }
  WorkerHostGuard(const WorkerHostGuard&) = delete;
  WorkerHostGuard& operator=(const WorkerHostGuard&) = delete;

 private:
  PresentationHost& presentation_;
  CoverOverlay& overlay_;
};

// Removes the per-run scratch directory (Edge's --user-data-dir is a complete
// browser profile) on every RunWorker exit path, including the early returns.
class TempDirGuard {
 public:
  explicit TempDirGuard(std::filesystem::path path) : path_(std::move(path)) {}
  ~TempDirGuard() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDirGuard(const TempDirGuard&) = delete;
  TempDirGuard& operator=(const TempDirGuard&) = delete;

 private:
  std::filesystem::path path_;
};

// The legacy flag files live next to the config file, the way the VBS tooling
// and the reference docs expect. An empty result disables the mirror.
std::filesystem::path CompatBaseDir(const SupervisorRunOptions& options) {
  if (options.config_path.empty()) return {};
  const auto parent = std::filesystem::path(options.config_path).parent_path();
  return parent.empty() ? std::filesystem::path{} : parent;
}

// On every RunWorker exit path: flush the runtime journal into the legacy
// watch-log.txt (the only post-mortem the VBS-era tooling reads) and remove the
// flag files this worker owns. The lock is left to the supervisor when the
// worker is killed, matching the "stale lock" rule of the legacy watcher.
class CompatRunGuard {
 public:
  CompatRunGuard(CompatFlags* flags, LegacyLog* log,
                 const std::vector<std::string>* journal)
      : flags_(flags), log_(log), journal_(journal) {}
  ~CompatRunGuard() {
    if (log_ != nullptr) {
      if (journal_ != nullptr) {
        for (const auto& line : *journal_) log_->Append(kWatchLog, line);
      }
      log_->Append(kWatchLog, "native watchdog stop");
    }
    if (flags_ != nullptr) flags_->DeleteAllOwned();
  }
  CompatRunGuard(const CompatRunGuard&) = delete;
  CompatRunGuard& operator=(const CompatRunGuard&) = delete;

 private:
  CompatFlags* flags_;
  LegacyLog* log_;
  const std::vector<std::string>* journal_;
};

DeckInfoResult LoadSupervisorDeckInfo(const SupervisorRunOptions& options,
                                      const Config& config) {
  const auto deck_path = ResolveDeckPath(config);
  if (!options.deck_info_path.empty()) {
    JsonDeckInfoProvider provider(options.deck_info_path);
    return provider.Load(deck_path);
  }
  SteadyClock clock;
  WpsHostConfig host_config;
  host_config.launch_attempts = 1;
  WpsHost host(clock, host_config);
  DeckInfoResult loaded = host.Load(deck_path);
  host.Release();
  return loaded;
}

// Writes BOM-free UTF-8 text, reporting failures without throwing.
bool WriteTextFile(const std::filesystem::path& path,
                   const std::string& text, std::string* error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    *error = "failed to open " + Utf8Path(path) + " for writing";
    return false;
  }
  output << text;
  if (text.empty() || text.back() != '\n') output.put('\n');
  output.close();
  if (!output) {
    *error = "failed to write " + Utf8Path(path);
    return false;
  }
  return true;
}

// Resolves the config once and writes the plan the worker will read, so target
// resolution never runs twice and the two roles cannot disagree. The effective
// config (profile, --map, and the other CLI overrides already applied) is
// written next to the plan: the worker derives its state machine from that
// file, so its timings cannot disagree with the plan. Returns false with a
// readable message on any failure, before anything is launched.
bool WriteSupervisorPlan(const SupervisorRunOptions& options,
                         const std::string& run_id,
                         const std::filesystem::path& state_dir,
                         std::string* error) {
  const auto loaded = LoadConfigFile(options.config_path, options.overrides);
  if (!loaded.config.has_value()) {
    *error = "cannot load config " + Utf8Path(options.config_path) + ": " +
             FirstError(loaded.issues);
    return false;
  }
  const Config& config = *loaded.config;

  DeckInfoResult deck = LoadSupervisorDeckInfo(options, config);
  if (!deck.info.has_value()) {
    *error = "cannot load deck info: " + FirstError(deck.issues);
    return false;
  }
  if (deck.info->slides.empty()) {
    *error = "deck info has no slides: " + Utf8Path(deck.info->path);
    return false;
  }

  ResolveOptions resolve_options;
  resolve_options.allow_outside_folder = options.overrides.allow_outside_folder;
  const ResolveResult resolved =
      ResolveTargets(config, &*deck.info, resolve_options);
  if (HasErrorIssue(resolved.issues)) {
    *error = "cannot resolve targets: " + FirstError(resolved.issues);
    return false;
  }

  const WorkerPlan plan =
      BuildWorkerPlan(config, resolved,
                      static_cast<int>(deck.info->slides.size()), run_id);
  std::string write_error;
  if (!WriteWorkerPlanFile(plan, state_dir / "worker-plan.json",
                           &write_error)) {
    *error = std::move(write_error);
    return false;
  }
  if (!WriteTextFile(state_dir / "worker-config.json", ConfigToJson(config),
                     &write_error)) {
    *error = std::move(write_error);
    return false;
  }
  return true;
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
  // The supervisor closes the event as its last cleanup step; wait for that
  // so --stop returns only after worker termination and journal flush.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  for (;;) {
    HANDLE probe = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
    if (probe == nullptr) return 0;
    CloseHandle(probe);
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "error: supervisor cleanup timed out for run " << run_id
                << "\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
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

  // Resolve the config and write the worker's plan before the worker is
  // launched, so a bad config fails the run here instead of inside a restarted
  // worker.
  std::string plan_error;
  if (!WriteSupervisorPlan(options, run_id, state_dir, &plan_error)) {
    AppendJournal(journal, run_id, "Plan", "plan failed", plan_error);
    std::cerr << "error: " << plan_error << "\n";
    blackbox.reset();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CloseHandle(stop_event);
    return 1;
  }
  AppendJournal(journal, run_id, "Plan", "plan", "worker plan written");

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
        state_dir.wstring(), L"--config", options.config_path.wstring()};
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
  AppendJournal(journal, run_id, "Worker", "start", "worker started");

  const auto create_dump = [&](std::uint32_t pid) -> bool {
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
    const BOOL created =
        CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &information);
    if (information.hProcess != nullptr) CloseHandle(information.hProcess);
    if (information.hThread != nullptr) CloseHandle(information.hThread);
    return created != FALSE;
  };

  const auto apply = [&](const std::vector<SupervisorAction>& actions)
      -> std::optional<int> {
    for (const auto& action : actions) {
      switch (action.kind) {
        case SupervisorActionKind::StartWorker:
          break;
        case SupervisorActionKind::RequestDump:
          if (worker.pid() != 0) {
            if (create_dump(worker.pid())) {
              AppendJournal(journal, run_id, "Worker", "dump",
                            "dump requested");
            } else {
              AppendJournal(journal, run_id, "Worker", "dump",
                            "dump request failed");
            }
          }
          break;
        case SupervisorActionKind::TerminateWorker:
          job.Terminate();
          if (channel != nullptr) {
            channel->Close();
            channel.reset();
          }
          server.Close();
          AppendJournal(journal, run_id, "Worker", "terminate",
                        "worker terminated");
          break;
        case SupervisorActionKind::RestartWorker:
          channel.reset();
          server.Close();
          if (!server.Create(SupervisorPipeName(run_id)) || !launch_worker()) {
            AppendJournal(journal, run_id, "Worker", "restart",
                          "worker restart failed");
            cleanup();
            return 1;
          }
          AppendJournal(journal, run_id, "Worker", "restart",
                        "worker restarted");
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

  // Set when the worker's final Cleanup frame arrives. Combined with exit code
  // 0 it is a clean terminal end, not a crash: the worker finished on its own
  // (machine-driven stop or SafeMode) and must not be dumped or restarted.
  bool cleanup_received = false;

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

    bool worker_exited = false;

    if (channel == nullptr) {
      if (!core.worker_running()) {
        // Waiting for the restart deadline (or for the core to stop); avoid a
        // busy loop while no worker can connect.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        channel = server.Accept(Duration{500});
        if (channel == nullptr && worker.WaitForExit(0)) {
          worker_exited = true;
        } else if (channel == nullptr) {
          // Accept failure or timeout closes the server handle; restore a
          // listening instance so a still-connecting worker can attach.
          server.Create(SupervisorPipeName(run_id));
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    }

    if (channel != nullptr) {
      const auto frame = channel->Receive(Duration{100});
      if (frame.has_value()) {
        switch (frame->header.type) {
          case FrameType::Heartbeat: {
            WorkerEvent event = MakeEvent(WorkerEventKind::Heartbeat,
                                          frame->header.sequence);
            // Progress travels in the payload; a worker without it still
            // heartbeats and is judged by sequence alone.
            if (const auto payload =
                    DecodeHeartbeatPayload(frame->payload);
                payload.has_value()) {
              event.progress = payload->progress;
            }
            if (auto code = apply(core.Handle(event)); code.has_value()) {
              return *code;
            }
            break;
          }
          case FrameType::Stop:
            // Worker-initiated stop: the core StopRequested path expects a
            // CleanupDone that this worker never sends, so terminate and exit
            // directly instead of waiting for StopComplete.
            job.Terminate();
            channel->Close();
            channel.reset();
            server.Close();
            cleanup();
            return 0;
          case FrameType::Cleanup:
            cleanup_received = true;
            // While stopping, Cleanup completes the requested stop. Otherwise
            // it is the first half of a machine-driven clean exit; the exit
            // code check below decides that path.
            if (auto code =
                    apply(core.Handle(MakeEvent(WorkerEventKind::CleanupDone)));
                code.has_value()) {
              return *code;
            }
            break;
          default:
            break;
        }
      } else if (channel->broken()) {
        // The peer is gone (or the stream is misaligned). A worker that just
        // sent Cleanup is expected to close the pipe and exit 0, so leave it
        // for the exit-code check below; anything else is a crash and is
        // terminated now.
        if (!cleanup_received) job.Terminate();
        channel->Close();
        channel.reset();
        server.Close();
        worker_exited = true;
      } else if (worker.WaitForExit(0)) {
        channel->Close();
        channel.reset();
        server.Close();
        worker_exited = true;
      }
    }

    if (worker_exited) {
      if (cleanup_received) {
        // "Cleanup received + exit 0" is the worker's clean terminal end: the
        // supervisor journals it and exits 0 without a dump or a restart. A
        // missing Cleanup or a non-zero code is still the crash path below.
        // The pipe is already closed here, so bound the wait and terminate a
        // worker that sent Cleanup but never exited.
        DWORD worker_code = 0;
        const bool exited = worker.WaitForExit(2000);
        if (exited && GetExitCodeProcess(worker.process(), &worker_code) != 0 &&
            worker_code == 0) {
          AppendJournal(journal, run_id, "Worker", "clean exit",
                        "worker exited after cleanup");
          cleanup();
          return 0;
        }
        if (!exited) job.Terminate();
      }
      AppendJournal(journal, run_id, "Worker", "exit", "worker exited");
      // This worker did not end cleanly, so the next process must not inherit
      // its Cleanup flag.
      cleanup_received = false;
      if (auto code = apply(core.Handle(MakeEvent(WorkerEventKind::Exited)));
          code.has_value()) {
        return *code;
      }
    }

    if (auto code = apply(core.Tick()); code.has_value()) {
      return *code;
    }
  }
}

int RunWorker(const SupervisorRunOptions& options) {
  const auto state_dir =
      options.state_dir.empty() ? DefaultStateDir() : options.state_dir;
  const std::string run_id =
      options.run_id.empty() ? NewRunId() : options.run_id;

  // The supervisor resolves the config once and passes it on, so the worker
  // derives the machine timings and the host timings from the same file.
  // The supervisor bakes the effective config (selected profile, --map, and
  // the other CLI overrides) into the state dir, so the worker's machine
  // timings match the plan it reads. A hand-run worker can still pass --config.
  const auto effective_config = state_dir / "worker-config.json";
  std::error_code config_error;
  const bool has_effective_config =
      std::filesystem::is_regular_file(effective_config, config_error);
  const std::filesystem::path config_path =
      has_effective_config ? effective_config : options.config_path;
  if (config_path.empty()) {
    std::cerr << "error: --config is required for --worker\n";
    return 1;
  }
  if (has_effective_config && !options.config_path.empty()) {
    // One line so a hand-run --worker is not silently using a different config
    // than the one it was given on the command line.
    std::cerr << "note: --worker uses " << Utf8Path(effective_config)
              << " instead of --config " << Utf8Path(options.config_path)
              << "\n";
  }
  const auto loaded =
      has_effective_config
          ? LoadConfigFile(config_path, CliConfigOverrides{})
          : LoadConfigFile(config_path, options.overrides);
  if (!loaded.config.has_value()) {
    std::cerr << "error: cannot load config " << Utf8Path(config_path) << ": "
              << FirstError(loaded.issues) << "\n";
    return 1;
  }
  const Config& config = *loaded.config;
  const StateMachineConfig machine_config = BuildStateMachineConfig(config);

  std::string plan_error;
  const auto plan =
      ReadWorkerPlanFile(state_dir / "worker-plan.json", &plan_error);
  if (!plan.has_value()) {
    std::cerr << "error: " << plan_error << "\n";
    return 1;
  }
  // A failed plan write in an earlier run can leave its file behind, so a
  // plan from another run is never executed.
  if (!ValidateWorkerPlanRunId(*plan, run_id, &plan_error)) {
    std::cerr << "error: " << plan_error << "\n";
    return 1;
  }

  auto blackbox = SharedBlackbox::OpenWritable(SupervisorBlackboxName(run_id));
  if (blackbox == nullptr) {
    std::cerr << "error: cannot open blackbox for run " << run_id << "\n";
    return 1;
  }

  NamedPipeClient client;
  if (!client.Connect(SupervisorPipeName(run_id), Duration{5000})) {
    std::cerr << "error: cannot connect to supervisor pipe\n";
    return 1;
  }

  SteadyClock clock;
  WpsHost wps(clock);
  Win32CoverOverlay overlay;
  Win32EdgeLauncher launcher;
  Win32WindowSystem windows;
  WorkerHostGuard host_guard(wps, overlay);

  WorkerHostFacts facts;
  // Edge's --user-data-dir is a complete browser profile (~15 MB per run), so
  // it lives in the system temp area, not in the state directory, and the guard
  // removes it when this worker exits.
  facts.temp_dir =
      std::filesystem::temp_directory_path() / ("htmlppt-" + run_id);
  const TempDirGuard temp_guard(facts.temp_dir);
  facts.screen = PrimaryScreenMetrics();
  if (const auto edge = FindEdgeExecutable(); edge.has_value()) {
    facts.edge_exe = *edge;
  } else {
    std::cerr << "warning: Edge was not found; Edge launches will fail\n";
  }

  // Opening the deck, starting the show, and collecting the slideshow pids can
  // block for seconds (COM activation plus WPS), which is longer than the
  // supervisor's heartbeat window. Heartbeats keep flowing while it happens.
  // The callback runs on this thread and only records facts: the blackbox is
  // single-writer, so its diagnostic records are appended after the startup
  // heartbeat thread has joined.
  struct StartupResult {
    bool launch_failed = false;
    long launch_error = 0;
    std::string launch_detail;
    bool show_failed = false;
    long show_error = 0;
    std::string show_detail;
    std::size_t wps_pid_count = 0;
  } started;
  const auto startup = [&] {
    const HostResult launched = wps.Launch(PathFromUtf8(plan->deck_utf8));
    if (!launched.ok()) {
      started.launch_failed = true;
      started.launch_error = launched.error;
      started.launch_detail = launched.detail;
      return;
    }
    // The machine leaves Warming only on ShowStarted, and Warming has no
    // deadline, so the host owes the opening show: without this the worker
    // would sit in Warming forever and never reach the cover/Edge flow.
    const HostResult shown = wps.StartShow();
    if (!shown.ok()) {
      started.show_failed = true;
      started.show_error = shown.error;
      started.show_detail = shown.detail;
      return;
    }
    facts.wps_pids = CollectWpsPids();
    started.wps_pid_count = facts.wps_pids.size();
  };

  // Legacy compatibility mirror: the VBS-era tooling looks for watcher.lock and
  // watch-log.txt next to the config, and stop-*.vbs refuses to run without the
  // lock. Only the worker touches these files while it owns the run.
  const auto compat_dir = CompatBaseDir(options);
  std::optional<CompatFlags> compat;
  std::optional<LegacyLog> legacy;
  std::atomic<std::int64_t> last_lock_ms{0};
  if (!compat_dir.empty()) {
    std::error_code compat_error;
    std::filesystem::create_directories(compat_dir, compat_error);
    compat.emplace(compat_dir,
                   CompatIdentity{run_id, GetCurrentProcessId(), ""});
    legacy.emplace(compat_dir);
    if (!compat->WriteLock(UtcTimestamp())) {
      std::cerr << "warning: cannot write " << kLockFile << " in "
                << Utf8Path(compat_dir) << "\n";
    }
    legacy->Append(kWatchLog, "native watchdog start run=" + run_id);
  }

  WorkerSessionConfig session_config;
  // Refresh the lock from the heartbeat thread: the legacy watcher treats a
  // lock older than 15 s as stale and may delete it.
  session_config.on_heartbeat = [&compat, &last_lock_ms] {
    if (!compat.has_value()) return;
    const auto now_ms =
        std::chrono::duration_cast<Duration>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (now_ms - last_lock_ms.load() < 5000) return;
    last_lock_ms.store(now_ms);
    compat->WriteLock(UtcTimestamp());
  };
  bool pipe_alive = true;
  session_config.initial_sequence = RunWithStartupHeartbeats(
      client, *blackbox, session_config, startup, &pipe_alive);
  if (!pipe_alive) {
    // The session reports the loss to the runtime and keeps running.
    std::cerr << "warning: supervisor pipe closed before the worker loop "
                 "started\n";
  }
  if (started.launch_failed) {
    AppendWorkerDiagnostic(*blackbox, BlackboxEvent::Failure,
                           started.launch_error,
                           "WPS launch failed: " + started.launch_detail);
    std::cerr << "error: cannot open " << plan->deck_utf8 << ": "
              << started.launch_detail << "\n";
    return 1;
  }
  if (started.show_failed) {
    AppendWorkerDiagnostic(*blackbox, BlackboxEvent::Failure, started.show_error,
                           "start show failed: " + started.show_detail);
    std::cerr << "error: cannot start the WPS slideshow: "
              << started.show_detail << "\n";
    return 1;
  }
  // Starting the show is a success fact worth seeing in --status: a regression
  // that drops StartShow must be visible in the blackbox, not just inferred.
  AppendWorkerDiagnostic(*blackbox, BlackboxEvent::Event, 0, "start show");
  const auto pid_count = static_cast<std::int64_t>(started.wps_pid_count);
  // An empty list is a warning, not a failure: some WPS builds expose no
  // slideshow window class, and the run continues with FocusWps inert.
  AppendWorkerDiagnostic(
      *blackbox, BlackboxEvent::Event, pid_count,
      pid_count == 0
          ? std::string{"wps pids: 0 (no slideshow window class)"}
          : "wps pids: " + std::to_string(pid_count));
  if (pid_count == 0) {
    std::cerr << "warning: no WPS slideshow window class found; focus and "
                 "return will not target WPS\n";
  }

  WorkerRuntime runtime(
      *plan, machine_config, WorkerPorts{wps, overlay, launcher, windows, clock},
      BuildWorkerHostConfig(config, facts));
  // Declared after the runtime so the journal is still alive when the guard
  // flushes it to watch-log.txt.
  CompatRunGuard compat_guard(compat ? &*compat : nullptr,
                              legacy ? &*legacy : nullptr,
                              &runtime.journal());

  std::string session_error;
  const int code = RunWorkerSession(runtime, client, *blackbox, session_config,
                                    &session_error);
  if (!session_error.empty()) {
    std::cerr << "warning: " << session_error << "\n";
  }
  return code;
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
