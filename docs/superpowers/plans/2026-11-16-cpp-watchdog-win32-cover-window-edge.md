# C++ Watchdog Win32 Cover, Window, Focus, and Edge Adapters (Plan 5B-2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Map the Plan 5B-1 platform-neutral cover, window, focus, and Edge seams onto Win32: a layered topmost overlay, `EnumWindows` plus foreground unlocking, UTF-8-safe `CreateProcessW` under per-launch Job Objects, and profile-based Edge window matching.

**Architecture:** Every adapter lives under `native/src/platform/win32/` and implements an existing interface from `core/`. The adapters convert UTF-8 strings to UTF-16 at the boundary, never use the active ANSI code page, and keep `core/` free of Windows headers. Automated tests run in `htmlppt_win32_tests` with real USER32 windows and a small helper executable; real Edge acceptance is an environment-gated test so CI and Linux/WSL remain deterministic.

**Tech Stack:** C++20, Win32 (USER32, GDI32, kernel32, shell32, ntdll), CMake/CTest, the existing test harness and `htmlppt_core`/`htmlppt_win32` targets.

## Global Constraints

- Target Windows 10/11 x64; Plan 5B-2 must keep the Linux/WSL build and tests green.
- C++20; MSVC `/W4 /permissive- /utf-8 /WX`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only. Use hex escapes for non-ASCII test data.
- `core/` must not include Windows headers. All Windows types stay under `platform/win32/`.
- Config strings are UTF-8. Convert them with `PathFromUtf8` for filesystem work and with `Utf8ToWide` for Win32 wide APIs. Never use `std::filesystem::path::string()` or the ANSI code page for Windows argv.
- Never push. Every task ends with a commit.
- Existing Linux gate: 244/244 unit cases, CTest 8/8, `npm test` 3/3.
- Existing Windows gate: `htmlppt_win32_tests` 28/28, CTest 9/9, `/WX` clean.
- Windows gate commands (from `native/README.md`):

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

## Scope

Plan 5B-2 implements:

- `Utf8ToWide` / `WideToUtf8` conversion and a `CommandLineToArgvW`-compatible
  Windows command-line builder.
- `Win32WindowSystem`: `EnumWindows` descriptors (PID, class, title, rect,
  visibility, topmost/popup/min-max styles), foreground queries, and the full
  legacy foreground-unlock strategy chain with result verification.
- `PrimaryScreenMetrics` for WPS ranking.
- Restoring the legacy 90 percent fullscreen hard gate in WPS ranking, as
  permitted by the Plan 5B-1 deferral.
- `Win32CoverOverlay`: a dedicated-thread layered topmost no-activate
  click-through window covering the virtual screen, with opacity, topmost
  reassertion, display-change handling, and a foreground-event topmost guard.
- `Win32EdgeLauncher` / `Win32EdgeProcess`: UTF-8-to-UTF-16 argv,
  `CreateProcessW`, per-launch Job Object, terminate, and released-ownership
  detach.
- `process_query`: Toolhelp process enumeration, command-line reads through
  `NtQueryInformationProcess`, and Edge window lookup by profile marker.
- Edge executable discovery.
- Windows integration tests for every adapter, plus one environment-gated real
  Edge acceptance test.

Not in Plan 5B-2: staging `file`/`tree`/`direct`, worker business loop,
state-machine wiring, compatibility flag mirroring, legacy launcher shims,
WinEvent-driven slide detection, and end-to-end WPS/Edge acceptance. These are
Plan 5C/5D.

## File Structure

```text
native/
  src/
    core/
      edge_controller.h/.cpp      # modify: destructor owns, Release() detaches
      focus_model.cpp             # modify: restore 90 percent fullscreen gate
    platform/win32/
      utf8_win32.h/.cpp           # UTF-8 <-> UTF-16
      process_job.h/.cpp          # modify: command-line builder
      window_system_win32.h/.cpp  # EnumWindows + foreground unlock
      cover_overlay_win32.h/.cpp  # layered topmost overlay thread
      edge_launcher_win32.h/.cpp  # CreateProcessW + Job ownership
      process_query.h/.cpp        # Toolhelp + command line + window marker
  tests/
    unit/
      edge_controller_test.cpp    # modify: release/destructor coverage
      focus_model_test.cpp        # modify: fullscreen hard gate
    integration/win32/
      utf8_win32_test.cpp
      window_system_win32_test.cpp
      cover_overlay_win32_test.cpp
      edge_launcher_win32_test.cpp
      process_job_test.cpp        # modify: command-line round trips
      helper/arg_echo.cpp         # test helper executable
  CMakeLists.txt                  # modify: shell32 and helper target
  README.md                       # modify: Plan 5B-2 section
```

The existing `CONFIGURE_DEPENDS` globs pick up new core and Win32 sources and
tests. `native/CMakeLists.txt` needs changes only for `shell32`, the helper
executable, and the helper path definition.

---

## Task 1: UTF-8 boundary and Windows command-line builder

**Files:**
- Create: `native/src/platform/win32/utf8_win32.h`
- Create: `native/src/platform/win32/utf8_win32.cpp`
- Create: `native/tests/integration/win32/utf8_win32_test.cpp`
- Modify: `native/src/platform/win32/process_job.h`
- Modify: `native/src/platform/win32/process_job.cpp`
- Modify: `native/tests/integration/win32/process_job_test.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Produces: `std::optional<std::wstring> Utf8ToWide(std::string_view)`
- Produces: `std::optional<std::string> WideToUtf8(std::wstring_view)`
- Produces: `std::wstring BuildWindowsCommandLine(const std::filesystem::path&, const std::vector<std::wstring>&)`

- [ ] **Step 1: Write the failing UTF-8 conversion tests**

Create `native/tests/integration/win32/utf8_win32_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32

#include <string>
#include <string_view>

#include "platform/win32/utf8_win32.h"

using htmlppt::Utf8ToWide;
using htmlppt::WideToUtf8;

HTMLPPT_TEST_CASE(win32_utf8_round_trips_ascii) {
  const auto wide = Utf8ToWide("C:/demo/page1.html");
  HTMLPPT_CHECK(wide.has_value());
  HTMLPPT_CHECK_EQ(*wide, std::wstring(L"C:/demo/page1.html"));
  const auto narrow = WideToUtf8(*wide);
  HTMLPPT_CHECK(narrow.has_value());
  HTMLPPT_CHECK_EQ(*narrow, std::string("C:/demo/page1.html"));
}

HTMLPPT_TEST_CASE(win32_utf8_round_trips_cjk_and_emoji) {
  // UTF-8 for two CJK characters followed by U+1F600.
  const std::string text =
      "\xE6\xBC\x94\xE7\xA4\xBA\xF0\x9F\x98\x80";
  const auto wide = Utf8ToWide(text);
  HTMLPPT_CHECK(wide.has_value());
  HTMLPPT_CHECK_EQ(wide->size(), std::size_t{4});
  const auto narrow = WideToUtf8(*wide);
  HTMLPPT_CHECK(narrow.has_value());
  HTMLPPT_CHECK_EQ(*narrow, text);
}

HTMLPPT_TEST_CASE(win32_utf8_rejects_invalid_utf8) {
  const std::string invalid("\xC3\x28", 2);
  HTMLPPT_CHECK(!Utf8ToWide(invalid).has_value());
}

HTMLPPT_TEST_CASE(win32_utf8_rejects_invalid_utf16) {
  const std::wstring invalid(1, static_cast<wchar_t>(0xD800));
  HTMLPPT_CHECK(!WideToUtf8(invalid).has_value());
}

HTMLPPT_TEST_CASE(win32_utf8_handles_empty_input) {
  const auto wide = Utf8ToWide(std::string_view());
  HTMLPPT_CHECK(wide.has_value());
  HTMLPPT_CHECK(wide->empty());
  const auto narrow = WideToUtf8(std::wstring_view());
  HTMLPPT_CHECK(narrow.has_value());
  HTMLPPT_CHECK(narrow->empty());
}

#endif // _WIN32
```

- [ ] **Step 2: Run the Windows tests and verify the new cases fail**

Run the Windows gate commands. Expected: the build fails because
`platform/win32/utf8_win32.h` does not exist.

- [ ] **Step 3: Implement the UTF-8 boundary**

Create `native/src/platform/win32/utf8_win32.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <optional>
#include <string>
#include <string_view>

namespace htmlppt {

// Converts UTF-8 bytes to UTF-16. Returns nullopt on invalid input or size
// overflow. An empty input produces an empty string.
std::optional<std::wstring> Utf8ToWide(std::string_view text);

// Converts UTF-16 to UTF-8. Returns nullopt on invalid input or size
// overflow. An empty input produces an empty string.
std::optional<std::string> WideToUtf8(std::wstring_view text);

} // namespace htmlppt

#endif // _WIN32
```

Create `native/src/platform/win32/utf8_win32.cpp`:

```cpp
#include "platform/win32/utf8_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <climits>

namespace htmlppt {

std::optional<std::wstring> Utf8ToWide(std::string_view text) {
  if (text.empty()) return std::wstring();
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int length = static_cast<int>(text.size());
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       text.data(), length, nullptr, 0);
  if (size <= 0) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  const int written = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size);
  if (written != size) return std::nullopt;
  return result;
}

std::optional<std::string> WideToUtf8(std::wstring_view text) {
  if (text.empty()) return std::string();
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
  const int length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       text.data(), length, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return std::nullopt;
  std::string result(static_cast<std::size_t>(size), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size,
      nullptr, nullptr);
  if (written != size) return std::nullopt;
  return result;
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Write the failing command-line round-trip tests**

Append these cases to `native/tests/integration/win32/process_job_test.cpp`
before `#endif // _WIN32`, and add `#include <shellapi.h>` after
`#include <windows.h>`:

```cpp
using htmlppt::BuildWindowsCommandLine;

namespace {

std::vector<std::wstring> ParseCommandLine(const std::wstring& line) {
  int count = 0;
  LPWSTR* parsed = CommandLineToArgvW(line.c_str(), &count);
  if (parsed == nullptr) return {};
  std::vector<std::wstring> result(parsed, parsed + count);
  LocalFree(parsed);
  return result;
}

} // namespace

HTMLPPT_TEST_CASE(win32_command_line_round_trips_arguments) {
  const std::filesystem::path exe =
      L"C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe";
  const std::vector<std::wstring> arguments = {
      L"--app=file:///C:/demo/page.html",
      L"plain",
      L"\x6F22\x5B57",
  };
  const std::wstring line = BuildWindowsCommandLine(exe, arguments);
  const auto parsed = ParseCommandLine(line);
  HTMLPPT_CHECK_EQ(parsed.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(parsed[0], exe.wstring());
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    HTMLPPT_CHECK_EQ(parsed[i + 1], arguments[i]);
  }
}

HTMLPPT_TEST_CASE(win32_command_line_escapes_quotes_and_backslashes) {
  const std::filesystem::path exe = L"C:\\Tools\\arg echo.exe";
  const std::vector<std::wstring> arguments = {
      L"say \"hi\"",
      L"C:\\path with space\\",
      L"plain\\path",
  };
  const std::wstring line = BuildWindowsCommandLine(exe, arguments);
  const auto parsed = ParseCommandLine(line);
  HTMLPPT_CHECK_EQ(parsed.size(), std::size_t{4});
  HTMLPPT_CHECK_EQ(parsed[0], exe.wstring());
  HTMLPPT_CHECK_EQ(parsed[1], arguments[0]);
  HTMLPPT_CHECK_EQ(parsed[2], arguments[1]);
  HTMLPPT_CHECK_EQ(parsed[3], arguments[2]);
}

HTMLPPT_TEST_CASE(win32_command_line_preserves_empty_argument) {
  const std::wstring line =
      BuildWindowsCommandLine(L"C:\\helper.exe", {L"", L"x"});
  const auto parsed = ParseCommandLine(line);
  HTMLPPT_CHECK_EQ(parsed.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(parsed[1], std::wstring());
  HTMLPPT_CHECK_EQ(parsed[2], std::wstring(L"x"));
}
```

- [ ] **Step 5: Build and verify the command-line tests fail**

Run the Windows gate. Expected: `BuildWindowsCommandLine` is undefined.

- [ ] **Step 6: Implement the command-line builder**

In `native/src/platform/win32/process_job.h`, add this declaration after
`IsProcessAlive` and before `#endif // _WIN32`:

```cpp
// Builds a CreateProcessW command line using the CommandLineToArgvW quoting
// rules. The executable and every argument are quoted only when needed;
// embedded quotes and trailing backslashes are escaped.
std::wstring BuildWindowsCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments);
```

In `native/src/platform/win32/process_job.cpp`, replace the anonymous
`Quote` helper with `QuoteArgument` and add `BuildWindowsCommandLine` after
`IsProcessAlive`:

```cpp
namespace {

std::wstring QuoteArgument(const std::wstring& value) {
  const bool needs_quotes =
      value.empty() || value.find_first_of(L" \t\"") != std::wstring::npos;
  std::wstring result;
  if (needs_quotes) result.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  if (needs_quotes) {
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
  } else {
    result.append(backslashes, L'\\');
  }
  return result;
}

} // namespace
```

Replace the command-line construction in `LaunchSuspended` with:

```cpp
  const std::wstring command_line =
      BuildWindowsCommandLine(exe, args);
  std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
  buffer.push_back(L'\0');
```

Add this definition after `IsProcessAlive` and before the closing namespace:

```cpp
std::wstring BuildWindowsCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments) {
  std::wstring line = QuoteArgument(executable.wstring());
  for (const auto& argument : arguments) {
    line.push_back(L' ');
    line += QuoteArgument(argument);
  }
  return line;
}
```

Add `shell32` to the Win32 library link line in `native/CMakeLists.txt`
because the new command-line tests call `CommandLineToArgvW`:

```cmake
  target_link_libraries(htmlppt_win32 PUBLIC htmlppt_core dbghelp ole32
    oleaut32 shell32)
```

- [ ] **Step 7: Run the Windows tests and verify all Task 1 cases pass**

Run the Windows gate. Expected: `htmlppt_win32_tests` 36/36, CTest 9/9,
`/WX` clean.

- [ ] **Step 8: Run the Linux gate to confirm no core regression**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: `htmlppt_core_tests` 244/244, CTest 8/8.

- [ ] **Step 9: Commit**

```bash
git add native/src/platform/win32/utf8_win32.h \
  native/src/platform/win32/utf8_win32.cpp \
  native/src/platform/win32/process_job.h \
  native/src/platform/win32/process_job.cpp \
  native/tests/integration/win32/utf8_win32_test.cpp \
  native/tests/integration/win32/process_job_test.cpp \
  native/CMakeLists.txt
git commit -m "feat: add utf8 and windows command line helpers"
```

---

## Task 2: Fullscreen hard gate and Win32 window system

**Files:**
- Modify: `native/src/core/focus_model.cpp`
- Modify: `native/tests/unit/focus_model_test.cpp`
- Create: `native/src/platform/win32/window_system_win32.h`
- Create: `native/src/platform/win32/window_system_win32.cpp`
- Create: `native/tests/integration/win32/window_system_win32_test.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `Utf8ToWide` / `WideToUtf8` from Task 1
- Produces: `ScreenMetrics PrimaryScreenMetrics()`
- Produces: `class Win32WindowSystem final : public WindowSystem`

- [ ] **Step 1: Write the failing WPS fullscreen-gate test**

Append to `native/tests/unit/focus_model_test.cpp`:

```cpp
HTMLPPT_TEST_CASE(focus_rejects_wps_window_below_fullscreen_gate) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(1, 300, "screenClass", WpsShowTitle(), 1700, 1080, true,
                 false, false),
  };
  HTMLPPT_CHECK(!SelectWpsShowWindow(windows, {300}, screen).has_value());
}
```

- [ ] **Step 2: Run the Linux tests and verify the new case fails**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
```

Expected: `focus_rejects_wps_window_below_fullscreen_gate` fails because the
1700-pixel-wide window still scores above the threshold.

- [ ] **Step 3: Restore the legacy fullscreen gate**

In `native/src/core/focus_model.cpp`, replace the final screen block and its
trailing `return score;` in `ScoreWpsWindow`:

```cpp
  if (screen.width > 0 && screen.height > 0) {
    const int min_width = static_cast<int>(screen.width * 0.9);
    const int min_height = static_cast<int>(screen.height * 0.9);
    if (window.rect.width() < min_width || window.rect.height() < min_height) {
      return 0;
    }
    score += 1;
  }
  return score;
```

- [ ] **Step 4: Run the Linux tests and verify they pass**

```bash
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
```

Expected: 245/245 pass. The existing
`focus_accepts_exactly_four_points` case remains valid because a fullscreen
window still receives the near-fullscreen bonus.

- [ ] **Step 5: Write the failing Win32 window-system tests**

Create `native/tests/integration/win32/window_system_win32_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "platform/win32/window_system_win32.h"

using htmlppt::PrimaryScreenMetrics;
using htmlppt::WindowId;
using htmlppt::WindowInfo;
using htmlppt::Win32WindowSystem;

namespace {

constexpr wchar_t kTestClassName[] = L"HtmlpptWindowSystemTest";

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                LPARAM lparam) {
  if (message == WM_DESTROY) return 0;
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void RegisterTestClass() {
  static std::once_flag once;
  std::call_once(once, [] {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = TestWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = kTestClassName;
    RegisterClassW(&window_class);
  });
}

HWND MakeTestWindow(const wchar_t* title, DWORD style, DWORD ex_style,
                    bool show) {
  RegisterTestClass();
  HWND hwnd = CreateWindowExW(ex_style, kTestClassName, title, style, 100, 100,
                              640, 480, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (show) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
  }
  return hwnd;
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

const WindowInfo* FindWindow(const std::vector<WindowInfo>& windows,
                             WindowId id) {
  for (const auto& window : windows) {
    if (window.id == id) return &window;
  }
  return nullptr;
}

bool InteractiveWindowStation() {
  USEROBJECTFLAGS flags{};
  DWORD length = 0;
  return GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS,
                                   &flags, sizeof(flags),
                                   &length) != 0 &&
         (flags.dwFlags & WSF_VISIBLE) != 0;
}

} // namespace

HTMLPPT_TEST_CASE(win32_window_system_enumerates_visible_window) {
  const HWND hwnd = MakeTestWindow(L"htmlppt window title",
                                   WS_OVERLAPPEDWINDOW, 0, true);
  HTMLPPT_CHECK(hwnd != nullptr);
  PumpMessages();

  Win32WindowSystem windows;
  const auto listed = windows.Enumerate();
  const auto* found = FindWindow(listed, reinterpret_cast<WindowId>(hwnd));
  HTMLPPT_CHECK(found != nullptr);
  HTMLPPT_CHECK_EQ(found->pid,
                   static_cast<std::uint32_t>(GetCurrentProcessId()));
  HTMLPPT_CHECK_EQ(found->title, std::string("htmlppt window title"));
  HTMLPPT_CHECK_EQ(found->class_name, std::string("HtmlpptWindowSystemTest"));
  HTMLPPT_CHECK(found->visible);
  HTMLPPT_CHECK(found->rect.width() > 0);
  HTMLPPT_CHECK(found->rect.height() > 0);
  HTMLPPT_CHECK(!found->topmost);
  HTMLPPT_CHECK(!found->ws_popup);
  HTMLPPT_CHECK(found->has_min_box);
  HTMLPPT_CHECK(found->has_max_box);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_reports_popup_and_topmost_styles) {
  const HWND hwnd =
      MakeTestWindow(L"popup", WS_POPUP, WS_EX_TOPMOST, true);
  HTMLPPT_CHECK(hwnd != nullptr);
  PumpMessages();

  Win32WindowSystem windows;
  const auto listed = windows.Enumerate();
  const auto* found = FindWindow(listed, reinterpret_cast<WindowId>(hwnd));
  HTMLPPT_CHECK(found != nullptr);
  HTMLPPT_CHECK(found->topmost);
  HTMLPPT_CHECK(found->ws_popup);
  HTMLPPT_CHECK(!found->has_min_box);
  HTMLPPT_CHECK(!found->has_max_box);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_marks_hidden_window_invisible) {
  const HWND hwnd =
      MakeTestWindow(L"hidden", WS_OVERLAPPEDWINDOW, 0, false);
  HTMLPPT_CHECK(hwnd != nullptr);

  Win32WindowSystem windows;
  const auto listed = windows.Enumerate();
  const auto* found = FindWindow(listed, reinterpret_cast<WindowId>(hwnd));
  HTMLPPT_CHECK(found != nullptr);
  HTMLPPT_CHECK(!found->visible);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_activates_own_window) {
  if (!InteractiveWindowStation()) return;
  const HWND original = GetForegroundWindow();
  const HWND hwnd =
      MakeTestWindow(L"activate", WS_OVERLAPPEDWINDOW, 0, true);
  HTMLPPT_CHECK(hwnd != nullptr);
  PumpMessages();

  Win32WindowSystem windows;
  const WindowId id = reinterpret_cast<WindowId>(hwnd);
  HTMLPPT_CHECK(windows.IsWindow(id));
  HTMLPPT_CHECK(windows.Activate(id));
  HTMLPPT_CHECK(windows.IsForeground(id));
  HTMLPPT_CHECK_EQ(windows.Foreground(), id);

  if (original != nullptr) SetForegroundWindow(original);
  DestroyWindow(hwnd);
}

HTMLPPT_TEST_CASE(win32_window_system_rejects_invalid_window) {
  Win32WindowSystem windows;
  HTMLPPT_CHECK(!windows.IsWindow(0));
  HTMLPPT_CHECK(!windows.IsForeground(0));
  HTMLPPT_CHECK(!windows.Activate(0));
}

HTMLPPT_TEST_CASE(win32_window_system_reports_primary_screen) {
  const auto screen = PrimaryScreenMetrics();
  HTMLPPT_CHECK(screen.width > 0);
  HTMLPPT_CHECK(screen.height > 0);
}

#endif // _WIN32
```

- [ ] **Step 6: Build and verify the window-system tests fail**

Run the Windows gate. Expected: the build fails because
`platform/win32/window_system_win32.h` does not exist.

- [ ] **Step 7: Implement the Win32 window system**

Create `native/src/platform/win32/window_system_win32.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <vector>

#include "core/window_system.h"

namespace htmlppt {

ScreenMetrics PrimaryScreenMetrics();

class Win32WindowSystem final : public WindowSystem {
 public:
  std::vector<WindowInfo> Enumerate() override;
  WindowId Foreground() override;
  bool IsForeground(WindowId id) override;
  bool Activate(WindowId id) override;
  bool IsWindow(WindowId id) override;
};

} // namespace htmlppt

#endif // _WIN32
```

Create `native/src/platform/win32/window_system_win32.cpp`:

```cpp
#include "platform/win32/window_system_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/win32/utf8_win32.h"

namespace htmlppt {
namespace {

HWND ToWindow(WindowId id) { return reinterpret_cast<HWND>(id); }
WindowId ToId(HWND hwnd) { return reinterpret_cast<WindowId>(hwnd); }

std::string WindowText(HWND hwnd) {
  const int length = GetWindowTextLengthW(hwnd);
  if (length <= 0) return {};
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied =
      GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
  if (copied <= 0) return {};
  text.resize(static_cast<std::size_t>(copied));
  return WideToUtf8(text).value_or(std::string());
}

std::string ClassName(HWND hwnd) {
  wchar_t buffer[256] = {};
  const int copied = GetClassNameW(hwnd, buffer, 256);
  if (copied <= 0) return {};
  const std::wstring_view name(buffer, static_cast<std::size_t>(copied));
  return WideToUtf8(name).value_or(std::string());
}

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM parameter) {
  auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(parameter);
  WindowInfo info;
  info.id = ToId(hwnd);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  info.pid = static_cast<std::uint32_t>(pid);
  info.class_name = ClassName(hwnd);
  info.title = WindowText(hwnd);
  RECT rect{};
  if (GetWindowRect(hwnd, &rect) != 0) {
    info.rect = WindowRect{rect.left, rect.top, rect.right, rect.bottom};
  }
  info.visible = IsWindowVisible(hwnd) != 0;
  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  info.topmost = (ex_style & WS_EX_TOPMOST) != 0;
  info.ws_popup = (style & WS_POPUP) != 0;
  info.has_min_box = (style & WS_MINIMIZEBOX) != 0;
  info.has_max_box = (style & WS_MAXIMIZEBOX) != 0;
  windows->push_back(std::move(info));
  return TRUE;
}

bool IsForegroundWindow(HWND hwnd) {
  return hwnd != nullptr && IsWindowVisible(hwnd) != 0 &&
         GetForegroundWindow() == hwnd;
}

} // namespace

ScreenMetrics PrimaryScreenMetrics() {
  ScreenMetrics metrics;
  metrics.width = GetSystemMetrics(SM_CXSCREEN);
  metrics.height = GetSystemMetrics(SM_CYSCREEN);
  return metrics;
}

std::vector<WindowInfo> Win32WindowSystem::Enumerate() {
  std::vector<WindowInfo> windows;
  EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&windows));
  return windows;
}

WindowId Win32WindowSystem::Foreground() {
  return ToId(GetForegroundWindow());
}

bool Win32WindowSystem::IsForeground(WindowId id) {
  return IsForegroundWindow(ToWindow(id));
}

bool Win32WindowSystem::IsWindow(WindowId id) {
  return id != 0 && ::IsWindow(ToWindow(id)) != 0;
}

bool Win32WindowSystem::Activate(WindowId id) {
  const HWND hwnd = ToWindow(id);
  if (hwnd == nullptr || ::IsWindow(hwnd) == 0) return false;

  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
  if (IsIconic(hwnd) != 0) ShowWindow(hwnd, SW_RESTORE);
  if (IsForegroundWindow(hwnd)) return true;

  SetForegroundWindow(hwnd);
  if (IsForegroundWindow(hwnd)) return true;

  SwitchToThisWindow(hwnd, TRUE);
  if (IsForegroundWindow(hwnd)) return true;

  const HWND foreground = GetForegroundWindow();
  const DWORD foreground_thread =
      foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  const DWORD target_thread = GetWindowThreadProcessId(hwnd, nullptr);
  const DWORD current_thread = GetCurrentThreadId();
  const bool attached_foreground =
      foreground_thread != 0 && foreground_thread != current_thread &&
      AttachThreadInput(current_thread, foreground_thread, TRUE) != 0;
  const bool attached_target =
      target_thread != 0 && target_thread != current_thread &&
      target_thread != foreground_thread &&
      AttachThreadInput(current_thread, target_thread, TRUE) != 0;
  BringWindowToTop(hwnd);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  if (attached_target) AttachThreadInput(current_thread, target_thread, FALSE);
  if (attached_foreground) {
    AttachThreadInput(current_thread, foreground_thread, FALSE);
  }
  if (IsForegroundWindow(hwnd)) return true;

  keybd_event(VK_MENU, 0, 0, 0);
  SetForegroundWindow(hwnd);
  SetFocus(hwnd);
  const bool alt_unlocked = IsForegroundWindow(hwnd);
  keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
  if (alt_unlocked) return true;

  SetForegroundWindow(hwnd);
  return IsForegroundWindow(hwnd);
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 8: Run the Windows tests and verify Task 2 passes**

Run the Windows gate. Expected: `htmlppt_win32_tests` 42/42, CTest 9/9,
`/WX` clean. If the activation test runs on a non-interactive window station,
it returns early and still reports PASS.

- [ ] **Step 9: Run the Linux gate**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: `htmlppt_core_tests` 245/245, CTest 8/8.

- [ ] **Step 10: Commit**

```bash
git add native/src/core/focus_model.cpp native/tests/unit/focus_model_test.cpp \
  native/src/platform/win32/window_system_win32.h \
  native/src/platform/win32/window_system_win32.cpp \
  native/tests/integration/win32/window_system_win32_test.cpp
git commit -m "feat: add win32 window system adapter"
```

---

## Task 3: Win32 layered cover overlay

**Files:**
- Create: `native/src/platform/win32/cover_overlay_win32.h`
- Create: `native/src/platform/win32/cover_overlay_win32.cpp`
- Create: `native/tests/integration/win32/cover_overlay_win32_test.cpp`

**Interfaces:**
- Consumes: `CoverOverlay` from `core/cover_overlay.h`
- Produces: `class Win32CoverOverlay final : public CoverOverlay`

- [ ] **Step 1: Write the failing overlay tests**

Create `native/tests/integration/win32/cover_overlay_win32_test.cpp`:

```cpp
#include "test_main.h"

#ifdef _WIN32

#include <windows.h>

#include "platform/win32/cover_overlay_win32.h"

using htmlppt::Win32CoverOverlay;

namespace {

int LayeredAlpha(HWND hwnd) {
  COLORREF color = 0;
  BYTE alpha = 0;
  DWORD flags = 0;
  if (GetLayeredWindowAttributes(hwnd, &color, &alpha, &flags) == 0) {
    return -1;
  }
  return (flags & LWA_ALPHA) != 0 ? static_cast<int>(alpha) : -1;
}

} // namespace

HTMLPPT_TEST_CASE(win32_cover_overlay_shows_layered_topmost_window) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.hwnd() != nullptr);
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  HTMLPPT_CHECK(IsWindowVisible(hwnd) != 0);

  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  HTMLPPT_CHECK((ex_style & WS_EX_LAYERED) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_TOOLWINDOW) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_NOACTIVATE) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_TRANSPARENT) != 0);
  HTMLPPT_CHECK((ex_style & WS_EX_TOPMOST) != 0);
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  HTMLPPT_CHECK((style & WS_POPUP) != 0);

  RECT rect{};
  HTMLPPT_CHECK(GetWindowRect(hwnd, &rect) != 0);
  HTMLPPT_CHECK(rect.right - rect.left > 0);
  HTMLPPT_CHECK(rect.bottom - rect.top > 0);
  overlay.Hide();
}

HTMLPPT_TEST_CASE(win32_cover_overlay_updates_and_clamps_opacity) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  HTMLPPT_CHECK(overlay.SetOpacity(0.5));
  HTMLPPT_CHECK_EQ(LayeredAlpha(hwnd), 128);
  HTMLPPT_CHECK(overlay.SetOpacity(1.5));
  HTMLPPT_CHECK_EQ(LayeredAlpha(hwnd), 255);
  HTMLPPT_CHECK(overlay.SetOpacity(-1.0));
  HTMLPPT_CHECK_EQ(LayeredAlpha(hwnd), 0);
  overlay.Hide();
}

HTMLPPT_TEST_CASE(win32_cover_overlay_hides_and_reshows) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  overlay.Hide();
  HTMLPPT_CHECK(IsWindowVisible(hwnd) == 0);
  HTMLPPT_CHECK(overlay.Show());
  HTMLPPT_CHECK(IsWindowVisible(hwnd) != 0);
  overlay.Hide();
}

HTMLPPT_TEST_CASE(win32_cover_overlay_reasserts_topmost) {
  Win32CoverOverlay overlay;
  HTMLPPT_CHECK(overlay.Show());
  const HWND hwnd = overlay.hwnd();
  SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  HTMLPPT_CHECK((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0);
  overlay.ReassertTopMost();
  HTMLPPT_CHECK((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
  overlay.Hide();
}

#endif // _WIN32
```

- [ ] **Step 2: Build and verify the overlay tests fail**

Run the Windows gate. Expected: the build fails because
`platform/win32/cover_overlay_win32.h` does not exist.

- [ ] **Step 3: Implement the overlay**

Create `native/src/platform/win32/cover_overlay_win32.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <thread>

#include "core/cover_overlay.h"

namespace htmlppt {

// Layered topmost no-activate click-through cover window. The window and its
// message pump live on a dedicated thread so CoverController ticks never
// block the state machine. A foreground-event hook reasserts topmost while
// the cover is visible.
class Win32CoverOverlay final : public CoverOverlay {
 public:
  Win32CoverOverlay();
  ~Win32CoverOverlay() override;

  Win32CoverOverlay(const Win32CoverOverlay&) = delete;
  Win32CoverOverlay& operator=(const Win32CoverOverlay&) = delete;

  bool Show() override;
  void Hide() override;
  bool SetOpacity(double opacity) override;
  void ReassertTopMost() override;

  HWND hwnd() const { return hwnd_.load(); }

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  static void CALLBACK ForegroundEvent(HWINEVENTHOOK hook, DWORD event,
                                       HWND hwnd, LONG object, LONG child,
                                       DWORD thread, DWORD time);
  void ThreadMain();
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void ResizeToVirtualScreen(HWND hwnd);
  void ApplyTopMost(HWND hwnd);

  std::atomic<HWND> hwnd_{nullptr};
  std::atomic<DWORD> thread_id_{0};
  bool visible_ = false;
  HANDLE ready_event_ = nullptr;
  std::thread thread_;
};

} // namespace htmlppt

#endif // _WIN32
```

Create `native/src/platform/win32/cover_overlay_win32.cpp`:

```cpp
#include "platform/win32/cover_overlay_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <utility>

namespace htmlppt {
namespace {

constexpr UINT kShowMessage = WM_APP + 1;
constexpr UINT kHideMessage = WM_APP + 2;
constexpr UINT kOpacityMessage = WM_APP + 3;
constexpr UINT kTopMostMessage = WM_APP + 4;
constexpr wchar_t kOverlayClass[] = L"HtmlpptCoverOverlay";
std::atomic<HWND> g_overlay_hwnd{nullptr};

} // namespace

Win32CoverOverlay::Win32CoverOverlay() {
  ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ready_event_ == nullptr) return;
  thread_ = std::thread(&Win32CoverOverlay::ThreadMain, this);
  WaitForSingleObject(ready_event_, 5000);
}

Win32CoverOverlay::~Win32CoverOverlay() {
  if (thread_.joinable()) {
    const DWORD id = thread_id_.load();
    if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
    thread_.join();
  }
  if (ready_event_ != nullptr) CloseHandle(ready_event_);
}

bool Win32CoverOverlay::Show() {
  const HWND hwnd = hwnd_.load();
  if (hwnd == nullptr) return false;
  return SendMessageW(hwnd, kShowMessage, 0, 0) != 0;
}

void Win32CoverOverlay::Hide() {
  const HWND hwnd = hwnd_.load();
  if (hwnd != nullptr) SendMessageW(hwnd, kHideMessage, 0, 0);
}

bool Win32CoverOverlay::SetOpacity(double opacity) {
  const HWND hwnd = hwnd_.load();
  if (hwnd == nullptr) return false;
  if (opacity < 0.0) opacity = 0.0;
  if (opacity > 1.0) opacity = 1.0;
  const BYTE alpha = static_cast<BYTE>(opacity * 255.0 + 0.5);
  return SendMessageW(hwnd, kOpacityMessage, alpha, 0) != 0;
}

void Win32CoverOverlay::ReassertTopMost() {
  const HWND hwnd = hwnd_.load();
  if (hwnd != nullptr) SendMessageW(hwnd, kTopMostMessage, 0, 0);
}

void Win32CoverOverlay::ThreadMain() {
  thread_id_.store(GetCurrentThreadId());
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = &Win32CoverOverlay::WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));
  window_class.lpszClassName = kOverlayClass;
  if (RegisterClassW(&window_class) == 0) {
    SetEvent(ready_event_);
    return;
  }

  HWND hwnd = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
          WS_EX_TRANSPARENT | WS_EX_TOPMOST,
      kOverlayClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
      window_class.hInstance, this);
  if (hwnd == nullptr) {
    UnregisterClassW(kOverlayClass, window_class.hInstance);
    DeleteObject(window_class.hbrBackground);
    SetEvent(ready_event_);
    return;
  }

  SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
  hwnd_.store(hwnd);
  g_overlay_hwnd.store(hwnd);
  const HWINEVENTHOOK hook = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
      &Win32CoverOverlay::ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
  SetEvent(ready_event_);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (hook != nullptr) UnhookWinEvent(hook);
  g_overlay_hwnd.store(nullptr);
  hwnd_.store(nullptr);
  DestroyWindow(hwnd);
  UnregisterClassW(kOverlayClass, window_class.hInstance);
  DeleteObject(window_class.hbrBackground);
}

void Win32CoverOverlay::ResizeToVirtualScreen(HWND hwnd) {
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  SetWindowPos(hwnd, nullptr, left, top, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

void Win32CoverOverlay::ApplyTopMost(HWND hwnd) {
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK Win32CoverOverlay::WindowProc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    auto* self = reinterpret_cast<Win32CoverOverlay*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return TRUE;
  }
  auto* self = reinterpret_cast<Win32CoverOverlay*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT Win32CoverOverlay::HandleMessage(HWND hwnd, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(hwnd, &paint);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(hwnd, &paint);
      return 0;
    }
    case kShowMessage:
      ResizeToVirtualScreen(hwnd);
      SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      ApplyTopMost(hwnd);
      visible_ = true;
      return TRUE;
    case kHideMessage:
      ShowWindow(hwnd, SW_HIDE);
      visible_ = false;
      return TRUE;
    case kOpacityMessage:
      if (!visible_) return FALSE;
      return SetLayeredWindowAttributes(hwnd, 0,
                                        static_cast<BYTE>(wparam),
                                        LWA_ALPHA) != 0
                 ? TRUE
                 : FALSE;
    case kTopMostMessage:
      if (visible_) ApplyTopMost(hwnd);
      return TRUE;
    case WM_DISPLAYCHANGE:
      ResizeToVirtualScreen(hwnd);
      if (visible_) ApplyTopMost(hwnd);
      return 0;
    case WM_CLOSE:
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

void CALLBACK Win32CoverOverlay::ForegroundEvent(HWINEVENTHOOK, DWORD, HWND,
                                                 LONG, LONG, DWORD, DWORD) {
  const HWND hwnd = g_overlay_hwnd.load();
  if (hwnd == nullptr || IsWindowVisible(hwnd) == 0) return;
  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 4: Run the Windows tests and verify Task 3 passes**

Run the Windows gate. Expected: `htmlppt_win32_tests` 46/46, CTest 9/9,
`/WX` clean. The overlay test briefly shows a black topmost window covering
the virtual screen and hides it immediately.

- [ ] **Step 5: Run the Linux gate**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: `htmlppt_core_tests` 245/245, CTest 8/8.

- [ ] **Step 6: Commit**

```bash
git add native/src/platform/win32/cover_overlay_win32.h \
  native/src/platform/win32/cover_overlay_win32.cpp \
  native/tests/integration/win32/cover_overlay_win32_test.cpp
git commit -m "feat: add win32 layered cover overlay"
```

---

## Task 4: Win32 Edge launcher, process query, and ownership

**Files:**
- Modify: `native/src/core/edge_controller.h`
- Modify: `native/src/core/edge_controller.cpp`
- Modify: `native/tests/unit/edge_controller_test.cpp`
- Create: `native/src/platform/win32/process_query.h`
- Create: `native/src/platform/win32/process_query.cpp`
- Create: `native/src/platform/win32/edge_launcher_win32.h`
- Create: `native/src/platform/win32/edge_launcher_win32.cpp`
- Create: `native/tests/integration/win32/helper/arg_echo.cpp`
- Create: `native/tests/integration/win32/edge_launcher_win32_test.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `Utf8ToWide`, `BuildWindowsCommandLine`, `Win32WindowSystem`
- Produces: `EdgeProcess::release()` and `EdgeController::~EdgeController()` / `EdgeController::Release()`
- Produces: `Win32EdgeLauncher`, `Win32EdgeProcess`
- Produces: `ProcessIdsByName`, `ProcessCommandLine`, `ProcessIdsByCommandLineMarker`, `FindWindowForProcessMarker`
- Produces: `FindExecutable`, `FindEdgeExecutable`

- [ ] **Step 1: Write the failing EdgeController ownership tests**

Replace `native/tests/unit/edge_controller_test.cpp` with:

```cpp
#include "test_main.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "core/edge_controller.h"

using htmlppt::EdgeController;
using htmlppt::EdgeLaunchSpec;
using htmlppt::EdgeLauncher;
using htmlppt::EdgeProcess;

namespace {

struct FakeState {
  bool alive = true;
  bool terminated = false;
  bool released = false;
};

class FakeEdgeProcess final : public EdgeProcess {
 public:
  FakeEdgeProcess(std::uint32_t pid, std::shared_ptr<FakeState> state)
      : pid_(pid), state_(std::move(state)) {}
  std::uint32_t pid() const override { return pid_; }
  bool alive() const override { return state_->alive; }
  void terminate() override {
    state_->alive = false;
    state_->terminated = true;
  }
  bool release() override {
    state_->released = true;
    return true;
  }

 private:
  std::uint32_t pid_ = 0;
  std::shared_ptr<FakeState> state_;
};

class FakeLauncher final : public EdgeLauncher {
 public:
  std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec&) override {
    ++launch_count;
    if (!succeed) return nullptr;
    last_state = std::make_shared<FakeState>();
    return std::make_unique<FakeEdgeProcess>(next_pid++, last_state);
  }
  int launch_count = 0;
  std::uint32_t next_pid = 100;
  bool succeed = true;
  std::shared_ptr<FakeState> last_state;
};

} // namespace

HTMLPPT_TEST_CASE(edge_controller_launches_and_kills) {
  FakeLauncher launcher;
  EdgeController controller(launcher);
  EdgeLaunchSpec spec;
  HTMLPPT_CHECK(controller.Launch(spec));
  HTMLPPT_CHECK(controller.active());
  HTMLPPT_CHECK_EQ(controller.pid(), std::uint32_t{100});
  controller.Kill();
  HTMLPPT_CHECK(!controller.active());
  HTMLPPT_CHECK_EQ(controller.pid(), std::uint32_t{0});
  HTMLPPT_CHECK(launcher.last_state->terminated);
}

HTMLPPT_TEST_CASE(edge_controller_rejects_second_launch_while_active) {
  FakeLauncher launcher;
  EdgeController controller(launcher);
  EdgeLaunchSpec spec;
  HTMLPPT_CHECK(controller.Launch(spec));
  HTMLPPT_CHECK(!controller.Launch(spec));
  HTMLPPT_CHECK_EQ(launcher.launch_count, 1);
}

HTMLPPT_TEST_CASE(edge_controller_handles_launch_failure) {
  FakeLauncher launcher;
  launcher.succeed = false;
  EdgeController controller(launcher);
  EdgeLaunchSpec spec;
  HTMLPPT_CHECK(!controller.Launch(spec));
  HTMLPPT_CHECK(!controller.active());
  HTMLPPT_CHECK_EQ(controller.pid(), std::uint32_t{0});
}

HTMLPPT_TEST_CASE(edge_controller_destructor_terminates_active_process) {
  FakeLauncher launcher;
  {
    EdgeController controller(launcher);
    HTMLPPT_CHECK(controller.Launch(EdgeLaunchSpec{}));
  }
  HTMLPPT_CHECK(launcher.last_state->terminated);
  HTMLPPT_CHECK(!launcher.last_state->alive);
}

HTMLPPT_TEST_CASE(edge_controller_release_detaches_without_terminating) {
  FakeLauncher launcher;
  {
    EdgeController controller(launcher);
    HTMLPPT_CHECK(controller.Launch(EdgeLaunchSpec{}));
    const auto state = launcher.last_state;
    HTMLPPT_CHECK(controller.Release());
    HTMLPPT_CHECK(state->released);
    HTMLPPT_CHECK(!state->terminated);
    HTMLPPT_CHECK(!controller.active());
    HTMLPPT_CHECK_EQ(controller.pid(), std::uint32_t{0});
  }
  HTMLPPT_CHECK(!launcher.last_state->terminated);
}
```

- [ ] **Step 2: Run the Linux tests and verify the new cases fail**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
```

Expected: the build fails because `EdgeProcess::release()` and
`EdgeController::Release()` do not exist.

- [ ] **Step 3: Implement EdgeController ownership**

Replace `native/src/core/edge_controller.h` with:

```cpp
#pragma once

#include <cstdint>
#include <memory>

#include "core/edge_spec.h"

namespace htmlppt {

class EdgeProcess {
 public:
  virtual ~EdgeProcess() = default;
  virtual std::uint32_t pid() const = 0;
  virtual bool alive() const = 0;
  virtual void terminate() = 0;
  // Drops ownership without terminating the process. Returns false when the
  // platform cannot detach; the controller keeps ownership in that case.
  virtual bool release() = 0;
};

class EdgeLauncher {
 public:
  virtual ~EdgeLauncher() = default;
  virtual std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) = 0;
};

class EdgeController {
 public:
  explicit EdgeController(EdgeLauncher& launcher);
  ~EdgeController();

  EdgeController(const EdgeController&) = delete;
  EdgeController& operator=(const EdgeController&) = delete;

  bool Launch(const EdgeLaunchSpec& spec);
  void Kill();
  // Detaches the process so it keeps running after this controller is
  // destroyed. Returns false when detaching failed.
  bool Release();
  bool active() const;
  std::uint32_t pid() const;

 private:
  EdgeLauncher& launcher_;
  std::unique_ptr<EdgeProcess> process_;
};

} // namespace htmlppt
```

Replace `native/src/core/edge_controller.cpp` with:

```cpp
#include "core/edge_controller.h"

namespace htmlppt {

EdgeController::EdgeController(EdgeLauncher& launcher) : launcher_(launcher) {}

EdgeController::~EdgeController() { Kill(); }

bool EdgeController::Launch(const EdgeLaunchSpec& spec) {
  if (active()) return false;
  process_ = launcher_.Launch(spec);
  return process_ != nullptr && process_->alive();
}

void EdgeController::Kill() {
  if (process_ != nullptr) {
    process_->terminate();
    process_.reset();
  }
}

bool EdgeController::Release() {
  if (process_ == nullptr) return true;
  if (!process_->release()) return false;
  process_.reset();
  return true;
}

bool EdgeController::active() const {
  return process_ != nullptr && process_->alive();
}

std::uint32_t EdgeController::pid() const {
  return process_ != nullptr ? process_->pid() : 0;
}

} // namespace htmlppt
```

- [ ] **Step 4: Run the Linux tests and verify they pass**

```bash
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
```

Expected: 247/247 pass.

- [ ] **Step 5: Add the helper executable and CMake wiring**

Create `native/tests/integration/win32/helper/arg_echo.cpp`:

```cpp
#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size,
                      nullptr, nullptr);
  return result;
}

LRESULT CALLBACK EchoWindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                LPARAM lparam) {
  if (message == WM_DESTROY) return 0;
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void WriteArguments(const std::filesystem::path& output, int argc,
                    wchar_t** argv) {
  std::ofstream stream(output, std::ios::binary);
  for (int index = 0; index < argc; ++index) {
    stream << "arg=" << ToUtf8(argv[index]) << "\n";
  }
  stream.flush();
  stream.close();
  std::ofstream ready(
      std::filesystem::path(output.wstring() + L".ready"), std::ios::binary);
  ready << "ready\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) return 2;
  const std::filesystem::path output(argv[1]);
  const int hold_ms = _wtoi(argv[2]);
  const bool want_window = std::wcscmp(argv[3], L"window") == 0;
  WriteArguments(output, argc, argv);

  if (!want_window) {
    Sleep(static_cast<DWORD>(hold_ms < 0 ? 0 : hold_ms));
    return 0;
  }

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = EchoWindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"HtmlpptArgEchoWindowClass";
  if (RegisterClassW(&window_class) == 0) return 3;

  HWND hwnd = CreateWindowExW(0, window_class.lpszClassName,
                              L"HtmlpptArgEchoWindow", WS_OVERLAPPEDWINDOW,
                              100, 100, 800, 600, nullptr, nullptr,
                              window_class.hInstance, nullptr);
  if (hwnd == nullptr) return 4;
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd);

  const ULONGLONG deadline =
      GetTickCount64() + static_cast<ULONGLONG>(hold_ms < 0 ? 0 : hold_ms);
  while (GetTickCount64() < deadline) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  DestroyWindow(hwnd);
  return 0;
}
```

Modify `native/CMakeLists.txt`:

1. Inside the existing `if (WIN32)` test block, before
`add_executable(htmlppt_win32_tests ...)`, add:

```cmake
    add_executable(htmlppt_win32_arg_echo
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/win32/helper/arg_echo.cpp")
    htmlppt_enable_warnings(htmlppt_win32_arg_echo)
```

2. Extend the test target definitions and dependencies:

```cmake
    target_compile_definitions(htmlppt_win32_tests PRIVATE
      "HTMLPPT_EXE_PATH=\"$<TARGET_FILE:htmlppt-watchdog>\""
      "HTMLPPT_HELPER_PATH=\"$<TARGET_FILE:htmlppt_win32_arg_echo>\"")
    add_dependencies(htmlppt_win32_tests htmlppt-watchdog
      htmlppt_win32_arg_echo)
```

- [ ] **Step 6: Write the failing Edge launcher and process-query tests**

Create `native/tests/integration/win32/edge_launcher_win32_test.cpp`:

```cpp
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
```

- [ ] **Step 7: Build and verify the Edge tests fail**

Run the Windows gate. Expected: the build fails because
`platform/win32/edge_launcher_win32.h` and `platform/win32/process_query.h`
do not exist.

- [ ] **Step 8: Implement process query and the Edge launcher**

Create `native/src/platform/win32/process_query.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/window_system.h"

namespace htmlppt {

std::vector<std::uint32_t> ProcessIdsByName(std::wstring_view image_name);

// Returns the full command line for a process when it can be queried.
std::optional<std::wstring> ProcessCommandLine(std::uint32_t pid);

// Returns processes whose executable name matches and whose command line
// contains the marker. Markers are compared case-sensitively.
std::vector<std::uint32_t> ProcessIdsByCommandLineMarker(
    std::wstring_view image_name, std::wstring_view marker);

// Finds the largest visible window owned by a process whose command line
// contains the marker. Used for per-profile Edge window matching.
std::optional<WindowId> FindWindowForProcessMarker(
    WindowSystem& windows, std::wstring_view image_name,
    std::wstring_view marker);

} // namespace htmlppt

#endif // _WIN32
```

Create `native/src/platform/win32/process_query.cpp`:

```cpp
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
```

Create `native/src/platform/win32/edge_launcher_win32.h`:

```cpp
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "core/edge_controller.h"
#include "platform/win32/process_job.h"

namespace htmlppt {

class Win32EdgeProcess final : public EdgeProcess {
 public:
  Win32EdgeProcess(ProcessHandle process, JobHandle job);
  ~Win32EdgeProcess() override;

  Win32EdgeProcess(const Win32EdgeProcess&) = delete;
  Win32EdgeProcess& operator=(const Win32EdgeProcess&) = delete;

  std::uint32_t pid() const override;
  bool alive() const override;
  void terminate() override;
  bool release() override;

 private:
  ProcessHandle process_;
  JobHandle job_;
};

class Win32EdgeLauncher final : public EdgeLauncher {
 public:
  std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) override;
};

// Returns the first existing regular file from `candidates`, then falls back
// to SearchPathW for `name`.
std::optional<std::filesystem::path> FindExecutable(
    std::wstring_view name,
    const std::vector<std::filesystem::path>& candidates);

std::optional<std::filesystem::path> FindEdgeExecutable();

} // namespace htmlppt

#endif // _WIN32
```

Create `native/src/platform/win32/edge_launcher_win32.cpp`:

```cpp
#include "platform/win32/edge_launcher_win32.h"

#ifdef _WIN32

#include <windows.h>

#include <system_error>
#include <utility>
#include <vector>

#include "platform/win32/utf8_win32.h"

namespace htmlppt {

Win32EdgeProcess::Win32EdgeProcess(ProcessHandle process, JobHandle job)
    : process_(std::move(process)), job_(std::move(job)) {}

Win32EdgeProcess::~Win32EdgeProcess() = default;

std::uint32_t Win32EdgeProcess::pid() const { return process_.pid(); }

bool Win32EdgeProcess::alive() const {
  return process_.pid() != 0 && IsProcessAlive(process_.pid());
}

void Win32EdgeProcess::terminate() {
  if (job_.valid()) job_.Terminate();
  if (process_.pid() != 0) {
    process_.Terminate();
    process_.WaitForExit(5000);
  }
}

bool Win32EdgeProcess::release() {
  if (process_.pid() == 0) return true;
  if (job_.valid()) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = 0;
    if (SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation,
                                &limits,
                                static_cast<DWORD>(sizeof(limits))) == 0) {
      return false;
    }
    job_ = JobHandle{};
  }
  process_ = ProcessHandle{};
  return true;
}

std::unique_ptr<EdgeProcess> Win32EdgeLauncher::Launch(
    const EdgeLaunchSpec& spec) {
  if (spec.executable.empty()) return nullptr;
  JobHandle job;
  if (!job.Create()) return nullptr;
  std::vector<std::wstring> arguments;
  arguments.reserve(spec.arguments.size());
  for (const auto& argument : spec.arguments) {
    auto wide = Utf8ToWide(argument);
    if (!wide.has_value()) return nullptr;
    arguments.push_back(std::move(*wide));
  }
  ProcessHandle process;
  if (!process.LaunchSuspended(spec.executable, arguments, job)) {
    return nullptr;
  }
  if (!process.Resume()) return nullptr;
  return std::make_unique<Win32EdgeProcess>(std::move(process),
                                            std::move(job));
}

std::optional<std::filesystem::path> FindExecutable(
    std::wstring_view name,
    const std::vector<std::filesystem::path>& candidates) {
  std::error_code error;
  for (const auto& candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    error.clear();
  }
  if (!name.empty()) {
    const std::wstring file_name(name);
    wchar_t found[MAX_PATH] = {};
    const DWORD length = SearchPathW(nullptr, file_name.c_str(), nullptr,
                                     MAX_PATH, found, nullptr);
    if (length > 0 && length < MAX_PATH) {
      return std::filesystem::path(std::wstring(found, length));
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> FindEdgeExecutable() {
  std::vector<std::filesystem::path> candidates;
  const wchar_t* variables[] = {L"ProgramFiles(x86)", L"ProgramFiles",
                                L"LOCALAPPDATA"};
  for (const wchar_t* variable : variables) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(variable, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) continue;
    candidates.push_back(std::filesystem::path(buffer) / L"Microsoft" /
                         L"Edge" / L"Application" / L"msedge.exe");
  }
  return FindExecutable(L"msedge.exe", candidates);
}

} // namespace htmlppt

#endif // _WIN32
```

- [ ] **Step 9: Run the Windows tests and verify Task 4 passes**

Run the Windows gate. Expected: `htmlppt_win32_tests` 54/54, CTest 9/9,
`/WX` clean. The manual Edge case returns early without
`HTMLPPT_MANUAL_EDGE`, so it still reports PASS.

- [ ] **Step 10: Run the Linux gate**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: `htmlppt_core_tests` 247/247, CTest 8/8.

- [ ] **Step 11: Commit**

```bash
git add native/src/core/edge_controller.h native/src/core/edge_controller.cpp \
  native/tests/unit/edge_controller_test.cpp \
  native/src/platform/win32/process_query.h \
  native/src/platform/win32/process_query.cpp \
  native/src/platform/win32/edge_launcher_win32.h \
  native/src/platform/win32/edge_launcher_win32.cpp \
  native/tests/integration/win32/edge_launcher_win32_test.cpp \
  native/tests/integration/win32/helper/arg_echo.cpp \
  native/CMakeLists.txt
git commit -m "feat: add win32 edge launcher and process query"
```

---

## Task 5: Documentation, full gates, and manual acceptance

**Files:**
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-16-cpp-watchdog-win32-cover-window-edge.md`

- [ ] **Step 1: Update the native README**

Insert this section before the `## WPS COM host (Plan 5A)` section in
`native/README.md`:

```markdown
## Win32 cover, window, focus, and Edge adapters (Plan 5B-2)

`platform/win32/utf8_win32` converts UTF-8 config and argv strings to UTF-16
with `MB_ERR_INVALID_CHARS`; `platform/win32/process_job` builds
`CommandLineToArgvW`-compatible command lines and escapes embedded quotes and
trailing backslashes. Windows argv never passes through the active code page.

`platform/win32/window_system_win32` enumerates top-level windows with PID,
class, title, rectangle, visibility, topmost/popup, and min/max style flags.
`Activate` follows the legacy chain: topmost plus restore, `SetForegroundWindow`,
`SwitchToThisWindow`, `AttachThreadInput` with `BringWindowToTop`, a synthetic
Alt tap, and a final foreground check. `PrimaryScreenMetrics` supplies the WPS
ranking screen size. WPS ranking restores the legacy 90 percent fullscreen
hard gate.

`platform/win32/cover_overlay_win32` owns a dedicated overlay thread with a
layered topmost `WS_POPUP` window, `WS_EX_NOACTIVATE`, `WS_EX_TOOLWINDOW`, and
`WS_EX_TRANSPARENT`, covering the virtual screen. `Show`, `Hide`, `SetOpacity`,
and `ReassertTopMost` are synchronous messages to that thread.
`EVENT_SYSTEM_FOREGROUND` reasserts topmost while the cover is visible, and
`WM_DISPLAYCHANGE` recomputes the virtual-screen bounds.

`platform/win32/edge_launcher_win32` launches Edge through `CreateProcessW`
under a per-launch Job Object with kill-on-close, converting every UTF-8
argument to UTF-16 first. `Win32EdgeProcess` reports liveness, terminates the
job, and `release()` clears kill-on-close and closes ownership so a
return-failure Edge can stay open for manual close. `core/edge_controller`
now terminates an owned process in its destructor and exposes `Release()`.

`platform/win32/process_query` enumerates processes with
`CreateToolhelp32Snapshot`, reads command lines through
`NtQueryInformationProcess(ProcessCommandLineInformation)`, and finds the
largest visible window for a process whose command line contains an Edge
profile marker. `FindEdgeExecutable` checks the Program Files, Program Files
(x86), and LocalAppData Edge locations before `SearchPathW`.

Windows gate: `htmlppt_core_tests` 245/245, `htmlppt_win32_tests` 54/54,
CTest 9/9, `/WX` clean. The real-Edge case is environment-gated:

```powershell
$env:HTMLPPT_MANUAL_EDGE = '1'
C:\Temp\htmlplppt-win\build\Release\htmlppt_win32_tests.exe
Remove-Item Env:HTMLPPT_MANUAL_EDGE
```
```

- [ ] **Step 2: Run the full Linux gate**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
CHROME_BIN=/home/bingmoe/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome npm test
```

Expected: `htmlppt_core_tests` 247/247, CTest 8/8, `npm test` 3/3,
`-Werror` clean.

- [ ] **Step 3: Run the full Windows gate**

Run the Windows gate commands from the Global Constraints section. Expected:
`htmlppt_core_tests` 245/245, `htmlppt_win32_tests` 54/54, CTest 9/9,
`/WX` clean.

- [ ] **Step 4: Record manual acceptance**

Attempt these on the Windows target machine:

1. The overlay cases briefly show a black topmost window covering the virtual
   screen and hide it. Confirm no black window remains after the suite.
2. Run the real-Edge case with `HTMLPPT_MANUAL_EDGE=1`. Confirm a fullscreen
   Edge app window opens, becomes foreground, and closes after the test.
3. After both runs, confirm no helper or `msedge.exe` process remains whose
   command line contains `htmlppt-manual-` or `htmlppt-marker-`.
4. If the machine lacks Edge or a real desktop, record manual acceptance as
   deferred instead of claiming it passed.

- [ ] **Step 5: Append the as-built amendments**

Append a `## As-Built Amendments (2026-11-16)` section to this plan with:

- the exact Linux and Windows gate counts observed;
- the actual commit hashes;
- every deviation from the code blocks above;
- the manual acceptance status, including whether the real-Edge case ran;
- deferred items, including staging, the worker loop, state-machine wiring,
  compatibility flag mirroring, and WinEvent-driven slide detection.

Do not leave the section empty and do not claim a manual check that was not
run.

- [ ] **Step 6: Commit**

```bash
git add native/README.md \
  docs/superpowers/plans/2026-11-16-cpp-watchdog-win32-cover-window-edge.md
git commit -m "docs: record Plan 5B-2 win32 adapters"
```

---

## Self-Review

### Plan 5B-2 spec coverage

| Spec requirement | Plan 5B-2 coverage |
|---|---|
| UTF-8 config and argv to UTF-16, never ANSI | Task 1 `utf8_win32`, Task 4 launcher |
| `CreateProcessW` argv quoting | Task 1 `BuildWindowsCommandLine` |
| Layered topmost no-activate click-through fullscreen cover | Task 3 overlay |
| Fade opacity, show/hide, topmost reassertion | Task 3 overlay |
| Overlay topmost guard after foreground changes | Task 3 `EVENT_SYSTEM_FOREGROUND` |
| Multi-monitor virtual-screen cover and display changes | Task 3 virtual-screen bounds and `WM_DISPLAYCHANGE` |
| `EnumWindows` descriptors with PID, title, class, rect, styles | Task 2 `Win32WindowSystem::Enumerate` |
| Foreground unlock strategy chain and verification | Task 2 `Win32WindowSystem::Activate` |
| WPS ranking screen metrics and fullscreen gate | Task 2 `PrimaryScreenMetrics` and `ScoreWpsWindow` |
| Edge launch under a Job Object | Task 4 `Win32EdgeLauncher` |
| Edge process alive, terminate, and keep-alive release | Task 4 `Win32EdgeProcess` |
| Edge profile exact window matching | Task 4 `FindWindowForProcessMarker` |
| Edge executable discovery | Task 4 `FindEdgeExecutable` |
| Automated Windows coverage of adapters | Tasks 1-4 integration tests |
| Real Edge acceptance | Task 5 environment-gated test |
| Staging, worker loop, state-machine wiring, compatibility mirror | Plan 5C/5D |

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.
- The only environment-conditional step is the documented real-Edge case.

### Type consistency

- `Utf8ToWide` and `WideToUtf8` return `std::optional` and are used by the
  window system, launcher, process query, and tests.
- `BuildWindowsCommandLine` is declared in `process_job.h` and used by
  `ProcessHandle::LaunchSuspended`.
- `EdgeProcess::release()` is pure virtual and implemented by the fake and
  `Win32EdgeProcess`; `EdgeController::~EdgeController` and
  `EdgeController::Release()` match the tests.
- `Win32CoverOverlay` implements all four `CoverOverlay` methods and keeps its
  own thread and window.
- `FindWindowForProcessMarker` takes `WindowSystem&` and returns
  `std::optional<WindowId>`, matching `Win32WindowSystem`.
- `HTMLPPT_HELPER_PATH` is defined on `htmlppt_win32_tests` and used only by
  the Edge launcher test.

### Expected test counts

- Linux `htmlppt_core_tests`: 244 + 1 fullscreen gate + 2 EdgeController
  ownership = 247.
- Windows `htmlppt_core_tests`: 247 minus the two pre-existing journal cases
  = 245.
- Windows `htmlppt_win32_tests`: 28 existing + 5 UTF-8 + 3 command-line +
  6 window-system + 4 overlay + 8 Edge/process = 54. The expected count is
  54/54; the README text and as-built section must use the observed count if
  the executed suite differs by a documented amendment.
- CTest: 8 on Linux, 9 on Windows.

---

## Execution Handoff

Plan 5B-2 is ready. Create a fresh worktree from `main`, commit this plan,
execute Tasks 1-4 with task-level reviews, run Task 5 gates and manual
acceptance, then run the final whole-branch review. Plan 5C (staging, Edge
execution flow, compatibility flags/logs, stop entry) follows after 5B-2
lands.
