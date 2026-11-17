#include "platform/win32/process_query.h"

#ifdef _WIN32

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace htmlppt {
namespace {

constexpr ULONG kProcessCommandLineInformation = 60;

using NtQueryInformationProcessFn =
    LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationProcessFn ResolveNtQueryInformationProcess() {
  static const NtQueryInformationProcessFn function = [] {
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) {
      return static_cast<NtQueryInformationProcessFn>(nullptr);
    }
    const FARPROC raw = GetProcAddress(module, "NtQueryInformationProcess");
    NtQueryInformationProcessFn result = nullptr;
    static_assert(sizeof(result) == sizeof(raw), "function pointer size");
    std::memcpy(&result, &raw, sizeof(result));
    return result;
  }();
  return function;
}

bool SameImageName(std::wstring_view left, std::wstring_view right) {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

} // namespace

std::vector<std::uint32_t> ProcessIdsByName(std::wstring_view image_name) {
  std::vector<std::uint32_t> result;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return result;
  PROCESSENTRY32W entry{};
  entry.dwSize = static_cast<DWORD>(sizeof(entry));
  if (Process32FirstW(snapshot, &entry) != 0) {
    do {
      if (entry.th32ProcessID != 0 &&
          SameImageName(entry.szExeFile, image_name)) {
        result.push_back(static_cast<std::uint32_t>(entry.th32ProcessID));
      }
    } while (Process32NextW(snapshot, &entry) != 0);
  }
  CloseHandle(snapshot);
  return result;
}

std::optional<std::wstring> ProcessCommandLine(std::uint32_t pid) {
  const auto query = ResolveNtQueryInformationProcess();
  if (query == nullptr || pid == 0) return std::nullopt;
  const HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return std::nullopt;

  ULONG size = 0;
  LONG status = query(process, kProcessCommandLineInformation, nullptr, 0,
                      &size);
  if (status >= 0 || size == 0) {
    CloseHandle(process);
    return std::nullopt;
  }
  std::unique_ptr<unsigned char[]> buffer(new unsigned char[size]);
  ULONG returned = size;
  status = query(process, kProcessCommandLineInformation, buffer.get(), size,
                 &returned);
  CloseHandle(process);
  if (status < 0) return std::nullopt;

  const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.get());
  if (text->Buffer == nullptr ||
      text->Length % static_cast<USHORT>(sizeof(wchar_t)) != 0) {
    return std::nullopt;
  }
  return std::wstring(text->Buffer,
                      text->Length / static_cast<USHORT>(sizeof(wchar_t)));
}

std::vector<std::uint32_t> ProcessIdsByCommandLineMarker(
    std::wstring_view image_name, std::wstring_view marker) {
  std::vector<std::uint32_t> result;
  if (marker.empty()) return result;
  for (const auto pid : ProcessIdsByName(image_name)) {
    const auto command_line = ProcessCommandLine(pid);
    if (command_line.has_value() &&
        command_line->find(marker) != std::wstring::npos) {
      result.push_back(pid);
    }
  }
  return result;
}

std::optional<WindowId> FindWindowForProcessMarker(
    WindowSystem& windows, std::wstring_view image_name,
    std::wstring_view marker) {
  const auto pids = ProcessIdsByCommandLineMarker(image_name, marker);
  if (pids.empty()) return std::nullopt;
  std::optional<WindowId> best;
  long long best_area = -1;
  for (const auto& window : windows.Enumerate()) {
    if (!window.visible) continue;
    if (std::find(pids.begin(), pids.end(), window.pid) == pids.end()) {
      continue;
    }
    const long long area = window.rect.area();
    if (area > best_area) {
      best_area = area;
      best = window.id;
    }
  }
  return best;
}

} // namespace htmlppt

#endif // _WIN32
