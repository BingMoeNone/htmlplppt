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
  HTMLPPT_CHECK(job.Assign(process.process()));
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
