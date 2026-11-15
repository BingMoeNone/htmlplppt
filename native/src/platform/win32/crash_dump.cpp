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
