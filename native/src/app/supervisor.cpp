#include "app/supervisor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/utf8_path.h"

#ifdef _WIN32
#include <windows.h>

#include "app/worker_session.h"
#include "core/blackbox.h"
#include "core/compat_flags.h"
#include "core/config.h"
#include "core/config_issue.h"
#include "core/deck_info.h"
#include "core/faults.h"
#include "core/journal.h"
#include "core/journal_reconcile.h"
#include "core/reload_policy.h"
#include "core/resolve.h"
#include "core/single_instance.h"
#include "core/state_machine.h"
#include "core/supervisor.h"
#include "core/version.h"
#include "core/worker_plan.h"
#include "core/worker_runtime.h"
#include "platform/win32/crash_dump.h"
#include "platform/win32/cover_overlay_win32.h"
#include "platform/win32/edge_launcher_win32.h"
#include "platform/win32/named_pipe.h"
#include "platform/win32/process_job.h"
#include "platform/win32/process_query.h"
#include "platform/win32/pumping_clock.h"
#include "platform/win32/screen_win32.h"
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

const char* ReconcileOutcomeName(ReconcileOutcome outcome) {
  switch (outcome) {
    case ReconcileOutcome::Clean:
      return "clean";
    case ReconcileOutcome::TakeOver:
      return "takeover";
    case ReconcileOutcome::Cleanup:
      return "cleanup";
    case ReconcileOutcome::RestoreWps:
      return "restore-wps";
    case ReconcileOutcome::SafeMode:
      return "safe-mode";
  }
  return "unknown";
}

// The tail is read before this run writes its start record, so every record in
// it belongs to a previous run — including a reused explicit --run-id (the smoke
// script always uses "smoke"). The most recent non-empty id names the Edge
// profile a crash can leave behind; an empty result means there is no previous
// run.
std::string PreviousRunId(const std::vector<JournalRecord>& tail) {
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
    if (!it->run_id.empty()) return it->run_id;
  }
  return {};
}

// Edge runs with --user-data-dir=%TEMP%\htmlppt-<run>\<profile>. Requiring the
// separator after the marker keeps run-1 from matching run-10, which the
// substring query alone would accept.
std::vector<std::uint32_t> LeftoverEdgePids(const std::string& previous_run_id) {
  if (previous_run_id.empty()) return {};
  const std::wstring marker = L"htmlppt-" + Widen(previous_run_id);
  const std::wstring profile_prefix = marker + L"\\";
  std::vector<std::uint32_t> pids;
  for (const auto pid : ProcessIdsByCommandLineMarker(L"msedge.exe", marker)) {
    const auto command_line = ProcessCommandLine(pid);
    if (!command_line.has_value()) continue;
    if (!CommandLineUsesProfile(*command_line, profile_prefix)) continue;
    pids.push_back(pid);
  }
  return pids;
}

// Reconciles the state a previous run may have left behind before the config is
// resolved. The facts describe only what this process can observe: a leftover
// Edge from the previous run (the tail names it; a fresh run id never appears in
// that run's marker), the legacy flags this run owns, and no overlay/WPS restore
// (a crashed worker's overlay dies with it, and restoring WPS at supervisor
// start is out of scope). The decision is journaled and never blocks startup:
// the normal plan/launch path follows and supersedes whatever was left behind.
void ReconcilePreviousRun(Journal& journal,
                          const std::vector<JournalRecord>& previous_tail,
                          const SupervisorRunOptions& options,
                          const std::string& run_id) {
  const std::string previous_run_id = PreviousRunId(previous_tail);
  const std::vector<std::uint32_t> edge_pids =
      LeftoverEdgePids(previous_run_id);

  ReconcileFacts facts;
  facts.edge_process_alive = !edge_pids.empty();
  facts.overlay_owned = false;
  facts.can_restore_wps = false;
  const auto compat_dir = CompatBaseDir(options);
  if (!compat_dir.empty()) {
    CompatFlags flags(compat_dir,
                      CompatIdentity{run_id, GetCurrentProcessId(), ""});
    facts.flags_owned_by_current_run = flags.IsOwned(kLockFile);
  }

  const ReconcileDecision decision =
      ReconcileStartup(previous_tail, facts, run_id);
  std::string detail = "reconcile: " +
                       std::string(ReconcileOutcomeName(decision.outcome));
  if (!previous_run_id.empty()) {
    detail += " previous=" + previous_run_id;
  }
  detail += " " + decision.reason;
  AppendJournal(journal, run_id, "Supervisor", "reconcile", detail);
  if (!decision.kill_edge) return;

  const std::size_t killed = TerminatePids(edge_pids);
  AppendJournal(journal, run_id, "Supervisor", "reconcile",
                "reconcile: killed " + std::to_string(killed) + " of " +
                    std::to_string(edge_pids.size()) + " edge processes");
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
                         std::string* error,
                         std::vector<std::string>* warnings) {
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

  WorkerPlan plan = BuildWorkerPlan(config, resolved,
                                    static_cast<int>(deck.info->slides.size()),
                                    run_id);
  // The plan is the only channel to the worker, so the display metrics for each
  // target must be resolved before the file is written. A run with no enabled
  // targets skips the fill entirely.
  if (!plan.machine.targets.empty()) {
    ResolveTargetScreens(plan.machine, config, plan.targets, warnings);
  }
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

void ResolveTargetScreens(RunPlan& plan, const Config& config,
                          const std::vector<ResolvedTarget>& resolved,
                          std::vector<std::string>* warnings) {
  // The plan's target and host-spec vectors are index aligned; the spec's
  // ordinal points at the source config target that owns display.monitor.
  const std::size_t count = std::min(plan.targets.size(), resolved.size());
  for (std::size_t i = 0; i < count; ++i) {
    const int ordinal = resolved[i].ordinal;
    if (ordinal < 1) continue;
    const auto index = static_cast<std::size_t>(ordinal - 1);
    if (index >= config.active.targets.size()) continue;
    const TargetConfig& source = config.active.targets[index];
    if (!source.display.has_value()) continue;
    const ScreenSelection selection =
        SelectScreenMetrics(source.display->monitor);
    if (!selection.error.empty() && warnings != nullptr) {
      warnings->push_back("target slide " +
                          std::to_string(plan.targets[i].slide) + " display: " +
                          selection.error);
    }
    if (selection.metrics.width <= 0 || selection.metrics.height <= 0) continue;
    plan.targets[i].screen_width = selection.metrics.width;
    plan.targets[i].screen_height = selection.metrics.height;
  }
}

std::size_t TerminatePids(std::span<const std::uint32_t> pids) {
  std::size_t terminated = 0;
  for (const auto pid : pids) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (process == nullptr) continue;
    if (TerminateProcess(process, 1)) ++terminated;
    CloseHandle(process);
  }
  return terminated;
}

bool CommandLineUsesProfile(const std::wstring& command_line,
                            const std::wstring& profile_prefix) {
  return command_line.find(profile_prefix) != std::wstring::npos;
}

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

int RunDiagnostics(std::string_view run_id,
                   const std::filesystem::path& state_dir,
                   const std::filesystem::path& out_dir) {
  if (out_dir.empty()) {
    std::cerr << "error: --diagnostics requires --out PATH\n";
    return 2;
  }
  std::error_code error;
  std::filesystem::create_directories(out_dir, error);
  if (error) {
    std::cerr << "error: cannot create " << Utf8Path(out_dir) << ": "
              << error.message() << "\n";
    return 1;
  }

  std::ostringstream manifest;
  manifest << "htmlppt-watchdog diagnostics\n";
  manifest << "version " << version() << "\n";
  manifest << "run " << run_id << "\n";
  manifest << "state_dir " << Utf8Path(state_dir) << "\n";
  manifest << "collected_at " << UtcTimestamp() << "\n";

  // Everything that explains a run is copied beside the manifest, so the
  // package can be handed over without the state directory.
  for (const char* name : {"journal.jsonl", "watch-log.txt", "worker-plan.json",
                           "worker-config.json", "watcher.json"}) {
    const auto source = state_dir / name;
    std::error_code copy_error;
    if (!std::filesystem::is_regular_file(source, copy_error)) continue;
    std::filesystem::copy_file(source, out_dir / name,
                               std::filesystem::copy_options::overwrite_existing,
                               copy_error);
    manifest << "file " << name << " " << (copy_error ? "failed" : "copied")
             << "\n";
  }

  // The black box only exists while the supervisor owns the run.
  if (auto blackbox = SharedBlackbox::Open(SupervisorBlackboxName(run_id));
      blackbox != nullptr) {
    const auto snapshot = blackbox->Snapshot();
    std::ofstream out(out_dir / "blackbox.txt", std::ios::binary);
    for (const auto& record : snapshot) {
      out << record.sequence << " " << record.timestamp_ms << " "
          << static_cast<int>(record.event) << " " << record.state << " "
          << record.detail << " ["
          << StateName(static_cast<State>(record.state)) << "]\n";
    }
    manifest << "blackbox records " << snapshot.size() << " appended "
             << blackbox->appended() << " dropped " << blackbox->dropped()
             << "\n";
  } else {
    manifest << "blackbox unavailable\n";
  }

  // Crash dumps the supervisor left next to the journal.
  for (const auto& entry :
       std::filesystem::directory_iterator(state_dir, error)) {
    if (error) break;
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".dmp") continue;
    std::error_code copy_error;
    std::filesystem::copy_file(entry.path(), out_dir / entry.path().filename(),
                               std::filesystem::copy_options::overwrite_existing,
                               copy_error);
    manifest << "dump " << Utf8Path(entry.path().filename()) << " "
             << (copy_error ? "failed" : "copied") << "\n";
  }

  std::ofstream manifest_file(out_dir / "manifest.txt", std::ios::binary);
  manifest_file << manifest.str();
  std::cout << "diagnostics " << Utf8Path(out_dir) << "\n";
  return 0;
}

int RunStatus(std::string_view run_id,
              const std::filesystem::path& state_dir) {  if (run_id.empty()) return 2;
  auto blackbox = SharedBlackbox::Open(SupervisorBlackboxName(run_id));
  if (blackbox == nullptr) {
    std::cout << "no blackbox for run " << run_id << "\n";
    return 1;
  }
  const auto snapshot = blackbox->Snapshot();
  // Spec 11.2: --status reports the state, run id, PID and last event, so a
  // stalled or failed run is diagnosable from one command.
  std::uint32_t worker_pid = 0;
  const char* state_name = "Unknown";
  std::string last_event;
  for (const auto& record : snapshot) {
    if (record.pid != 0) worker_pid = record.pid;
    state_name = StateName(static_cast<State>(record.state));
    last_event = record.detail;
  }
  std::cout << "run " << run_id << " state " << state_name << " pid "
            << worker_pid << " records " << snapshot.size() << " appended "
            << blackbox->appended() << " dropped " << blackbox->dropped()
            << "\n";
  if (!last_event.empty()) {
    std::cout << "last event " << last_event << "\n";
  }
  std::size_t start = 0;
  if (snapshot.size() > 10) start = snapshot.size() - 10;
  for (std::size_t i = start; i < snapshot.size(); ++i) {
    const auto& record = snapshot[i];
    std::cout << record.sequence << " " << record.timestamp_ms << " "
              << static_cast<int>(record.event) << " " << record.state
              << " " << record.detail << " ["
              << StateName(static_cast<State>(record.state)) << "]\n";
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
  // The engine reconciles the last state in the tail, so read the previous
  // run's records before this run writes its own "supervisor start" record:
  // that record would otherwise shadow the previous state and hide every
  // leftover the reconcile step exists to clean up.
  const std::vector<JournalRecord> previous_tail = journal.read_tail(50);
  AppendJournal(journal, run_id, "Supervisor", "start", "supervisor start");
  ReconcilePreviousRun(journal, previous_tail, options, run_id);

  // Resolve the config and write the worker's plan before the worker is
  // launched, so a bad config fails the run here instead of inside a restarted
  // worker.
  std::string plan_error;
  std::vector<std::string> plan_warnings;
  if (!WriteSupervisorPlan(options, run_id, state_dir, &plan_error,
                           &plan_warnings)) {
    // A failed plan is the run's terminal state, not a transient "Plan" step:
    // reconciliation treats Faulted as leftover state, while only a clean stop
    // may journal Exited.
    AppendJournal(journal, run_id, "Faulted", "plan failed", plan_error);
    std::cerr << "error: " << plan_error << "\n";
    blackbox.reset();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CloseHandle(stop_event);
    return 1;
  }
  for (const auto& warning : plan_warnings) {
    // Same "warn and journal" contract as the worker's global display path, so
    // an out-of-range per-target monitor is never a silent fallback.
    AppendJournal(journal, run_id, "Plan", "display warning", warning);
    std::cerr << "warning: " << warning << "\n";
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
  // Journals the run's terminal state before releasing its handles. Only a real
  // clean stop may journal "Exited": reconciliation treats that one state as
  // "the previous run released everything". A SafeMode/legacy end and a
  // launch/restart failure each journal their own state, so the next start
  // cleans up instead of trusting a run that did not stop cleanly.
  const auto cleanup = [&](std::string_view state = "Exited") {
    if (channel != nullptr) channel->Close();
    server.Close();
    job.Terminate();
    AppendJournal(journal, run_id, std::string(state), "stop",
                  "supervisor stop");
    blackbox.reset();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CloseHandle(stop_event);
  };

  if (!launch_worker()) {
    cleanup("Faulted");
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
            cleanup("Faulted");
            return 1;
          }
          AppendJournal(journal, run_id, "Worker", "restart",
                        "worker restarted");
          // A replacement process starts its heartbeat sequence over, so the
          // liveness trackers of the dead worker must not judge it: without
          // this resync every new heartbeat would look stale and a healthy
          // worker would be declared hung once the timeout passed.
          core.NoteWorkerRestarted(clock.now());
          break;
        case SupervisorActionKind::EnterSafeMode:
          AppendJournal(journal, run_id, "SafeMode", "action", action.detail);
          cleanup("SafeMode");
          return 3;
        case SupervisorActionKind::RunLegacyFallback:
          AppendJournal(journal, run_id, "SafeMode", "legacy", action.detail);
          cleanup("SafeMode");
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
  // True once the current request file's deferral was journalled, so a
  // transition lasting seconds cannot write one line per heartbeat. Reset when
  // the request disappears or is applied.
  bool reload_defer_journaled = false;

  // True once the current request file could not be cleared: a delete that keeps
  // failing would otherwise restart the worker on every heartbeat. While the
  // latch is set the request is ignored until the file disappears.
  bool reload_request_stuck = false;

  // The last worker state already journalled. Reconciliation reads the last
  // state record, so the worker's own states must reach journal.jsonl; the
  // cursor keeps a steady Monitoring loop from writing one record per heartbeat.
  std::optional<std::uint8_t> last_worker_state;

  // Hot reload: rebuild the plan exactly as startup does, then replace the
  // worker through the existing restart batch. A failed rebuild stops here and
  // keeps the old worker; the request is consumed so a broken config cannot
  // retry (and log) on every heartbeat, and a fresh --reload asks again. A plan
  // write that fails midway can leave worker-plan.json partially rewritten, so
  // the restart path must not reuse it without a successful rebuild.
  const auto apply_reload = [&]() -> std::optional<int> {
    const auto request_path = state_dir / "reload.request";
    std::string plan_error;
    std::vector<std::string> plan_warnings;
    if (!WriteSupervisorPlan(options, run_id, state_dir, &plan_error,
                             &plan_warnings)) {
      AppendJournal(journal, run_id, "Monitoring", "reload",
                    "reload: failed " + plan_error);
      std::cerr << "warning: reload failed: " << plan_error << "\n";
      std::error_code remove_error;
      if (!std::filesystem::remove(request_path, remove_error) ||
          remove_error) {
        reload_request_stuck = true;
        AppendJournal(journal, run_id, "Monitoring", "reload",
                      "reload: request not cleared");
      }
      return std::nullopt;
    }
    for (const auto& warning : plan_warnings) {
      AppendJournal(journal, run_id, "Monitoring", "reload",
                    "reload warning: " + warning);
      std::cerr << "warning: reload: " << warning << "\n";
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(request_path, remove_error) || remove_error) {
      // Without clearing the marker the next Monitoring heartbeat would apply
      // the same reload again, restarting the deck in a loop.
      reload_request_stuck = true;
      AppendJournal(journal, run_id, "Monitoring", "reload",
                    "reload: request not cleared");
      return std::nullopt;
    }
    AppendJournal(journal, run_id, "Monitoring", "reload", "reload: applied");
    // A worker that was about to exit cleanly must not make its replacement
    // look like a clean terminal end.
    cleanup_received = false;
    return apply({{SupervisorActionKind::TerminateWorker, "reload"},
                  {SupervisorActionKind::RestartWorker, "reload"}});
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
            // The payload is decoded once: progress feeds the liveness tracker
            // and the flags byte says whether the loop is really parked between
            // Steps, which is the only instant hot reload may apply.
            const auto payload = DecodeHeartbeatPayload(frame->payload);
            WorkerEvent event = MakeEvent(WorkerEventKind::Heartbeat,
                                          frame->header.sequence);
            // Progress travels in the payload; a worker without it still
            // heartbeats and is judged by sequence alone.
            if (payload.has_value()) {
              event.progress = payload->progress;
            }
            if (auto code = apply(core.Handle(event)); code.has_value()) {
              return *code;
            }

            // Journal the worker's states so reconciliation can see where a
            // crashed run really was. Transition-only: a steady Monitoring loop
            // would otherwise append one record per heartbeat and drown the
            // journal's signal.
            if (payload.has_value() &&
                (!last_worker_state.has_value() ||
                 *last_worker_state != payload->state)) {
              last_worker_state = payload->state;
              const std::string state_name =
                  StateName(static_cast<State>(payload->state));
              AppendJournal(journal, run_id, state_name, "state",
                            "worker state " + state_name);
            }

            // Hot reload (spec 9.6). --reload writes an empty request file;
            // the plan may only be swapped at a real safe point. The state byte
            // can still read Monitoring while a blocking Step is running (the
            // helper thread carries the last published state), so the decision
            // keys on the safe-point flag, not on `state == Monitoring`. Every
            // other instant is mid startup, switch/return or stop, so a request
            // waits there and is journalled once.
            std::error_code request_error;
            const bool request_pending = std::filesystem::exists(
                state_dir / "reload.request", request_error);
            const bool at_safe_point =
                payload.has_value() &&
                (payload->flags & kHeartbeatFlagSafePoint) != 0;
            const ReloadDecision decision =
                DecideReload(request_pending, at_safe_point);
            if (!request_pending) {
              reload_defer_journaled = false;
              reload_request_stuck = false;
            }
            if (decision == ReloadDecision::Defer) {
              if (ShouldJournalDefer(decision, reload_defer_journaled)) {
                reload_defer_journaled = true;
                const std::string state_name =
                    payload.has_value()
                        ? StateName(static_cast<State>(payload->state))
                        : std::string{"Unknown"};
                AppendJournal(journal, run_id, state_name, "reload",
                              "reload: deferred " + state_name);
              }
            } else if (decision == ReloadDecision::Apply &&
                       !reload_request_stuck) {
              reload_defer_journaled = false;
              if (auto code = apply_reload(); code.has_value()) {
                return *code;
              }
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

  // The worker loop's waits pump this thread's message queue: the loop is the
  // STA that owns the COM objects and the cover window, so a plain sleep would
  // starve their callbacks.
  PumpingClock clock;
  WpsHost wps(clock);
  Win32CoverOverlay overlay;
  Win32EdgeLauncher launcher;
  Win32WindowSystem windows;
  WorkerHostGuard host_guard(wps, overlay);

  WorkerHostFacts facts;
  // Spec 6.6 fault injection. The worker is launched by the supervisor, so the
  // switch travels through the environment; an injected run must be obvious in
  // the logs, never silent.
  if (const char* faults_env = std::getenv("HTMLPPT_FAULTS");
      faults_env != nullptr) {
    facts.faults = ParseFaults(faults_env);
  }
  if (HasAnyFault(facts.faults)) {
    AppendWorkerDiagnostic(*blackbox, BlackboxEvent::Event, 0,
                           "faults: " + FaultsToString(facts.faults));
    std::cerr << "warning: fault injection active: "
              << FaultsToString(facts.faults) << "\n";
  }
  // Edge's --user-data-dir is a complete browser profile (~15 MB per run), so
  // it lives in the system temp area, not in the state directory, and the guard
  // removes it when this worker exits.
  facts.temp_dir =
      std::filesystem::temp_directory_path() / ("htmlppt-" + run_id);
  const TempDirGuard temp_guard(facts.temp_dir);
  // Spec 8.2 display.monitor: the reference monitor for show-window scoring and
  // focus. The cover still spans the virtual screen, so every display goes dark.
  const ScreenSelection screen = SelectScreenMetrics(config.defaults.display.monitor);
  facts.screen = screen.metrics;
  if (!screen.error.empty()) {
    std::cerr << "warning: " << screen.error << "\n";
    AppendWorkerDiagnostic(*blackbox, BlackboxEvent::Event, 0,
                           "display: " + screen.error);
  }
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
    // Spec 8.5 prewarmProfile: initialize every target's Edge profile before
    // the show needs it. Runs inside the startup heartbeats, so the supervisor
    // keeps seeing liveness while the browser builds its profile.
    if (config.defaults.edge.prewarm_profile && !facts.edge_exe.empty()) {
      std::vector<std::unique_ptr<EdgeProcess>> prewarm;
      for (const auto& target : plan->targets) {
        const auto profile =
            facts.temp_dir / PathFromUtf8(target.edge_profile);
        auto process = launcher.Launch(
            MakeEdgePrewarmSpec(facts.edge_exe, profile, config.defaults.edge));
        if (process != nullptr) prewarm.push_back(std::move(process));
      }
      if (!prewarm.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        for (auto& process : prewarm) process->terminate();
      }
    }
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
  session_config.inject_supervisor_loss = facts.faults.supervisor_loss;
  bool pipe_alive = true;
  // Spec 11.3: stop-live.vbs deletes watcher.lock instead of sending a frame.
  // The session polls this at the loop cadence; the check itself is throttled so
  // a tight loop does not stat the file 50 times a second.
  std::atomic<std::int64_t> last_lock_check_ms{0};
  if (!compat_dir.empty()) {
    const auto lock_path = compat_dir / std::string(kLockFile);
    session_config.external_stop = [lock_path, &last_lock_check_ms] {
      const auto now_ms = NowMs();
      if (now_ms - last_lock_check_ms.load(std::memory_order_relaxed) < 500) {
        return false;
      }
      last_lock_check_ms.store(now_ms, std::memory_order_relaxed);
      std::error_code error;
      return !std::filesystem::exists(lock_path, error);
    };
  }
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

  // Legacy flag protocol (spec 11.3): mirror every loop state transition onto the
  // VBS-era flag files. RunWorker is the only writer; the session just hands each
  // post-step snapshot to this callback. The live target's Edge profile is still
  // in the snapshot when the machine enters Live, so it is resolved lazily.
  if (compat.has_value() && legacy.has_value()) {
    session_config.on_step = MakeCompatFlagMirror(
        *compat, *legacy,
        [&plan](const StateSnapshot& snapshot) -> std::string {
          if (!snapshot.current_target_slide.has_value()) return {};
          for (const TargetPlan& target : plan->machine.targets) {
            if (target.slide == *snapshot.current_target_slide) {
              return target.edge_profile;
            }
          }
          return {};
        },
        runtime.snapshot().state);
  }

  std::string session_error;
  const int code = RunWorkerSession(runtime, client, *blackbox, session_config,
                                    &session_error);
  if (runtime.snapshot().state == State::SafeMode) {
    // The machine's Diagnostic action is journal-only (the runtime has no
    // blackbox port); record it here so --status and the diagnostics package
    // show why the run ended in SafeMode.
    AppendWorkerDiagnostic(*blackbox, BlackboxEvent::Failure, 0,
                           "safe mode: worker entered SafeMode");
  }
  // --benchmark: report the loop and COM latencies next to the worker config, so
  // --diagnostics collects them and the acceptance doc's numbers are evidence.
  if (std::getenv("HTMLPPT_BENCHMARK") != nullptr) {
    const auto report_path =
        std::filesystem::path(options.config_path).parent_path() /
        "benchmark.txt";
    std::ofstream report(report_path, std::ios::binary);
    const auto& steps = runtime.step_stats();
    const auto& com = runtime.com_stats();
    report << "htmlppt-watchdog benchmark\n"
           << "run " << run_id << "\n"
           << "steps " << steps.count() << " p50 " << steps.P50().count()
           << "ms p99 " << steps.P99().count() << "ms max "
           << steps.Max().count() << "ms\n"
           << "com " << com.count() << " p50 " << com.P50().count()
           << "ms p99 " << com.P99().count() << "ms max " << com.Max().count()
           << "ms\n";
    std::cerr << "benchmark: steps p50 " << steps.P50().count() << "ms p99 "
              << steps.P99().count() << "ms; com p50 " << com.P50().count()
              << "ms p99 " << com.P99().count() << "ms\n";
  }
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

int RunDiagnostics(std::string_view, const std::filesystem::path&,
                   const std::filesystem::path&) {
  std::cerr << "error: --diagnostics requires Windows\n";
  return 2;
}

int RunDumpHelper(std::uint32_t, const std::filesystem::path&) {
  std::cerr << "error: --dump-helper requires Windows\n";
  return 2;
}

} // namespace htmlppt

#endif // _WIN32

namespace htmlppt {

// --reload is platform-neutral: the request is a file the supervisor polls, not
// a Win32 event. It needs no config (the run already owns one), only the run's
// state directory, which it creates when missing.
int RunReload(std::string_view run_id,
              const std::filesystem::path& state_dir) {
  if (run_id.empty() || state_dir.empty()) return 2;
  std::error_code error;
  std::filesystem::create_directories(state_dir, error);
  if (error) {
    std::cerr << "error: cannot create " << Utf8Path(state_dir) << ": "
              << error.message() << "\n";
    return 1;
  }
  const auto request = state_dir / "reload.request";
  // An empty file is the complete protocol: the supervisor only asks whether it
  // exists, so there is no partially written request to interpret.
  std::ofstream output(request, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    std::cerr << "error: cannot write " << Utf8Path(request) << "\n";
    return 1;
  }
  output.close();
  if (!output) {
    std::cerr << "error: cannot finish " << Utf8Path(request) << "\n";
    return 1;
  }
  std::cout << "reload requested\n";
  return 0;
}

} // namespace htmlppt
