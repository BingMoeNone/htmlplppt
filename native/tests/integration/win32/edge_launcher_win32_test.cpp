#include "test_main.h"

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/edge_controller.h"
#include "core/edge_spec.h"
#include "platform/win32/edge_launcher_win32.h"
#include "platform/win32/process_query.h"
#include "platform/win32/utf8_win32.h"
#include "platform/win32/window_system_win32.h"
#include "test_utils.h"

using htmlppt::BuildEdgeUrl;
using htmlppt::DefaultEdgeFlags;
using htmlppt::EdgeController;
using htmlppt::EdgeLaunchSpec;
using htmlppt::FindEdgeExecutable;
using htmlppt::FindExecutable;
using htmlppt::FindWindowForProcessMarker;
using htmlppt::IsProcessAlive;
using htmlppt::ProcessCommandLine;
using htmlppt::ProcessIdsByCommandLineMarker;
using htmlppt::ProcessIdsByName;
using htmlppt::ResolvedTarget;
using htmlppt::Utf8ToWide;
using htmlppt::WideToUtf8;
using htmlppt::Win32EdgeLauncher;
using htmlppt::Win32WindowSystem;

namespace {

std::filesystem::path HelperPath() {
  return std::filesystem::path(HTMLPPT_HELPER_PATH);
}

std::wstring UniqueMarker() {
  static std::atomic<unsigned> counter{0};
  return L"htmlppt-marker-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter++);
}

std::string ToUtf8(const std::filesystem::path& path) {
  return WideToUtf8(path.wstring()).value_or(std::string());
}

std::string ToUtf8(const std::wstring& text) {
  return WideToUtf8(text).value_or(std::string());
}

bool WaitForFile(const std::filesystem::path& path, int timeout_ms) {
  const ULONGLONG deadline =
      GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  while (GetTickCount64() < deadline) {
    if (std::filesystem::exists(path)) return true;
    Sleep(20);
  }
  return std::filesystem::exists(path);
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

bool ContainsLine(const std::vector<std::string>& lines,
                  const std::string& expected) {
  for (const auto& line : lines) {
    if (line == expected) return true;
  }
  return false;
}

void TerminatePid(std::uint32_t pid) {
  if (pid == 0) return;
  HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
  if (process == nullptr) return;
  TerminateProcess(process, 1);
  WaitForSingleObject(process, 5000);
  CloseHandle(process);
}

EdgeLaunchSpec HelperSpec(const std::filesystem::path& output, int hold_ms,
                          bool window, const std::wstring& marker,
                          const std::string& extra) {
  EdgeLaunchSpec spec;
  spec.executable = HelperPath();
  spec.url = "file:///unused";
  spec.profile_dir = output.parent_path();
  spec.arguments = {ToUtf8(output), std::to_string(hold_ms),
                    window ? "window" : "nowindow", ToUtf8(marker), extra};
  return spec;
}

} // namespace

HTMLPPT_TEST_CASE(win32_edge_launcher_passes_utf8_arguments) {
  htmlppt::test::TempDir directory;
  const auto output = directory.path() / L"args.txt";
  const std::wstring marker = UniqueMarker();
  const std::string extra = "\xE6\xBC\x94\xE7\xA4\xBA arg";
  Win32EdgeLauncher launcher;
  EdgeController controller(launcher);
  HTMLPPT_CHECK(controller.Launch(
      HelperSpec(output, 5000, false, marker, extra)));
  const std::uint32_t pid = controller.pid();
  HTMLPPT_CHECK(pid != 0);
  HTMLPPT_CHECK(WaitForFile(std::filesystem::path(output.wstring() + L".ready"),
                            5000));
  const auto lines = ReadLines(output);
  HTMLPPT_CHECK(ContainsLine(lines, "arg=" + ToUtf8(marker)));
  HTMLPPT_CHECK(ContainsLine(lines, "arg=" + extra));
  HTMLPPT_CHECK(ContainsLine(lines, "arg=nowindow"));
  controller.Kill();
  HTMLPPT_CHECK(!controller.active());
  HTMLPPT_CHECK(!IsProcessAlive(pid));
}

HTMLPPT_TEST_CASE(win32_edge_launcher_kills_process_job) {
  htmlppt::test::TempDir directory;
  const auto output = directory.path() / L"kill.txt";
  Win32EdgeLauncher launcher;
  EdgeController controller(launcher);
  HTMLPPT_CHECK(controller.Launch(
      HelperSpec(output, 10000, false, UniqueMarker(), "plain")));
  const std::uint32_t pid = controller.pid();
  HTMLPPT_CHECK(pid != 0);
  HTMLPPT_CHECK(controller.active());
  controller.Kill();
  HTMLPPT_CHECK(!controller.active());
  HTMLPPT_CHECK(!IsProcessAlive(pid));
}

HTMLPPT_TEST_CASE(win32_edge_controller_release_keeps_process_alive) {
  htmlppt::test::TempDir directory;
  const auto output = directory.path() / L"release.txt";
  Win32EdgeLauncher launcher;
  EdgeController controller(launcher);
  HTMLPPT_CHECK(controller.Launch(
      HelperSpec(output, 10000, false, UniqueMarker(), "plain")));
  const std::uint32_t pid = controller.pid();
  HTMLPPT_CHECK(pid != 0);
  HTMLPPT_CHECK(controller.Release());
  Sleep(200);
  HTMLPPT_CHECK(IsProcessAlive(pid));
  TerminatePid(pid);
}

HTMLPPT_TEST_CASE(win32_process_query_finds_current_process) {
  wchar_t module[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
  HTMLPPT_CHECK(length > 0);
  const std::wstring name =
      std::filesystem::path(std::wstring(module, length)).filename().wstring();
  const auto pids = ProcessIdsByName(name);
  bool found = false;
  for (const auto pid : pids) {
    if (pid == GetCurrentProcessId()) found = true;
  }
  HTMLPPT_CHECK(found);
}

HTMLPPT_TEST_CASE(win32_process_query_reads_command_line) {
  htmlppt::test::TempDir directory;
  const auto output = directory.path() / L"cmdline.txt";
  const std::wstring marker = UniqueMarker();
  Win32EdgeLauncher launcher;
  auto process = launcher.Launch(
      HelperSpec(output, 10000, false, marker, "plain"));
  HTMLPPT_CHECK(process != nullptr);
  HTMLPPT_CHECK(WaitForFile(std::filesystem::path(output.wstring() + L".ready"),
                            5000));
  const auto command_line = ProcessCommandLine(process->pid());
  HTMLPPT_CHECK(command_line.has_value());
  HTMLPPT_CHECK(command_line->find(marker) != std::wstring::npos);
  process->terminate();
}

HTMLPPT_TEST_CASE(win32_process_query_finds_window_by_marker) {
  htmlppt::test::TempDir directory;
  const auto output = directory.path() / L"window.txt";
  const std::wstring marker = UniqueMarker();
  Win32EdgeLauncher launcher;
  auto process = launcher.Launch(
      HelperSpec(output, 10000, true, marker, "plain"));
  HTMLPPT_CHECK(process != nullptr);
  HTMLPPT_CHECK(WaitForFile(std::filesystem::path(output.wstring() + L".ready"),
                            5000));
  Win32WindowSystem windows;
  std::optional<htmlppt::WindowId> found;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  while (GetTickCount64() < deadline && !found.has_value()) {
    found = FindWindowForProcessMarker(windows, HelperPath().filename().wstring(),
                                       marker);
    if (!found.has_value()) Sleep(50);
  }
  HTMLPPT_CHECK(found.has_value());
  bool pid_matches = false;
  bool visible = false;
  for (const auto& window : windows.Enumerate()) {
    if (window.id == *found) {
      pid_matches = window.pid == process->pid();
      visible = window.visible;
    }
  }
  HTMLPPT_CHECK(pid_matches);
  HTMLPPT_CHECK(visible);
  process->terminate();
}

HTMLPPT_TEST_CASE(win32_find_executable_prefers_existing_candidate) {
  htmlppt::test::TempDir directory;
  const auto found = FindExecutable(HelperPath().filename().wstring(),
                                    {HelperPath()});
  HTMLPPT_CHECK(found.has_value());
  HTMLPPT_CHECK_EQ(found->wstring(), HelperPath().wstring());
  const auto missing = FindExecutable(
      L"htmlppt-missing-7f3a.exe",
      {directory.path() / L"htmlppt-missing-7f3a.exe"});
  HTMLPPT_CHECK(!missing.has_value());
}

HTMLPPT_TEST_CASE(win32_edge_manual_launches_real_edge) {
  wchar_t enabled[8] = {};
  if (GetEnvironmentVariableW(L"HTMLPPT_MANUAL_EDGE", enabled, 8) == 0) return;
  const auto edge = FindEdgeExecutable();
  HTMLPPT_CHECK(edge.has_value());

  htmlppt::test::TempDir directory;
  const auto page = directory.path() / L"page.html";
  {
    std::ofstream stream(page, std::ios::binary);
    stream << "<!doctype html><title>manual edge</title><h1>edge</h1>";
  }
  const std::wstring profile =
      L"htmlppt-manual-" + std::to_wstring(GetCurrentProcessId());
  const auto profile_dir = directory.path() / profile;

  ResolvedTarget target;
  target.html_path = ToUtf8(page);
  EdgeLaunchSpec spec;
  spec.executable = *edge;
  spec.url = BuildEdgeUrl(target);
  spec.profile_dir = profile_dir;
  spec.arguments = DefaultEdgeFlags();
  spec.arguments.push_back("--app=" + spec.url);
  spec.arguments.push_back("--start-fullscreen");
  spec.arguments.push_back("--user-data-dir=" + ToUtf8(profile_dir));

  Win32EdgeLauncher launcher;
  EdgeController controller(launcher);
  HTMLPPT_CHECK(controller.Launch(spec));
  Win32WindowSystem windows;
  std::optional<htmlppt::WindowId> found;
  const ULONGLONG deadline = GetTickCount64() + 20000;
  while (GetTickCount64() < deadline && !found.has_value()) {
    found = FindWindowForProcessMarker(windows, L"msedge.exe", profile);
    if (!found.has_value()) Sleep(100);
  }
  HTMLPPT_CHECK(found.has_value());
  HTMLPPT_CHECK(windows.Activate(*found));
  HTMLPPT_CHECK(windows.IsForeground(*found));

  controller.Kill();
  const auto pids = ProcessIdsByCommandLineMarker(L"msedge.exe", profile);
  for (const auto pid : pids) TerminatePid(pid);
}

#endif // _WIN32
