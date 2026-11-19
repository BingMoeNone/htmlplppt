> **Superseded:** the master-detail plan at `docs/superpowers/plans/2026-11-17-cpp-watchdog-config-ui-v2.md` replaces this one. Tasks 1-4 of this plan are already committed (see that plan's "Already completed" table).

# Config UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `htmlppt-watchdog` a native `--config-ui` Win32 editor that loads `watcher.json`, edits its targets, validates them with the same resolver a run uses, and saves atomically with a backup — while the rest of the product keeps working unchanged.

**Architecture:** The UI is a thin view over the existing `ConfigEditorModel` (core, unit-tested). New model operations (`ApplyEdit`, `DisplayRows`, `SaveWithBackup`) are added first, each with tests; then a raw-Win32 window (`ConfigWindow`) lists the rows in a `ListView`, edits one row through a modal dialog, shows validation issues, and saves. The window is created and destroyed in an integration test; interactive behaviour is covered by the manual checklist added to `docs/07-acceptance.md`.

**Tech Stack:** C++20, Win32 + Common Controls v6 (`comctl32`), `IFileOpenDialog`-free v1 picker (`GetOpenFileNameW`), nlohmann/json (existing), existing core config/resolve model.

**Spec:** `docs/superpowers/specs/2026-11-14-cpp-watchdog-design.md` §9 (原生配置 UI). This plan implements §9.1/§9.2 (single-profile, table + detail dialog) and §9.4 (validation, atomic save, backup) only; the deferred parts are listed under "Out of scope" and must not be silently half-built.

## Global Constraints

- `src/core/` stays platform-neutral: no `windows.h`, and it must build on Linux.
- New code must be clean under GCC 16 `-Wall -Wextra -Wpedantic -Werror` and MSVC `/W4 /permissive-`.
- No MFC/ATL and no new third-party dependency; MinGW must keep compiling (link `comctl32`).
- Source files are UTF-8; UI strings are wide literals in the source (v1), not a resource DLL.
- The model is the single owner of edits; the view never mutates `Config` directly.
- Save is atomic: temp file + backup, then replace. A failed save must leave the original file intact.
- Every new behaviour gets a test in the same task; `ctest` (9 suites) and `npm test` stay green.
- ASCII-only for `scripts/*.ps1`/`*.vbs` (unchanged rule).

## Review Focus

These are the input classes most likely to bite a user, and each one is pinned by a test in the task that owns the code:

1. **Config file missing/unreadable at UI start** — the UI must open with a clear message and an empty model, never crash or silently overwrite the path. (Task 5)
2. **Save failure (read-only file, disk error)** — the existing file must survive byte-for-byte; the error is reported. (Task 3)
3. **Duplicate slide numbers / out-of-range slides** — `DisplayRows` marks the offending row, and Save is blocked while an error exists. (Task 4, Task 7)
4. **Non-ASCII paths in the file picker and save** — the picker returns UTF-16 and the model stores UTF-8; a CJK path round-trips. (Task 2, Task 6)
5. **Closing with unsaved edits** — the window prompts and can cancel the close. (Task 8)

## File Structure

- `native/src/core/config_editor.{h,cpp}` — model (existing); gains `TargetEdit`, `ApplyEdit`, `DisplayRow`/`DisplayRows`, `SaveWithBackup`.
- `native/src/app/config_ui.{h,cpp}` — command entry: load config, build the model, run the window (Windows-only; non-Windows stub returns 2).
- `native/src/platform/win32/config_window.{h,cpp}` — the Win32 view: main window, `ListView`, dialog, buttons, status line.
- `native/src/app/cli.cpp` + `native/README.md` — `--config-ui` flag, help text, docs.
- `native/tests/unit/config_editor_test.cpp` — model tests.
- `native/tests/integration/win32/config_window_test.cpp` — window lifecycle test.
- `docs/07-acceptance.md` — manual checklist for the interactive parts.

## Out of scope (do not implement in this plan)

- Auto-mapping wizard (§9.3) — separate plan.
- Undo/redo, drag-and-drop, thumbnails, dark/high-contrast theming, DWM custom chrome, keyboard navigation polish, `IFileOpenDialog`/`IFileSaveDialog`, hot reload while a run is active, launching/stopping the watchdog from the UI, WebView2 preview, profile/deck selectors, search/filter, deck fingerprint in the status bar (§9.2/§9.4/§9.5/§9.6).
- Saving a blocked edit as `<name>.draft.json` (§9.4): v1 reports the error and refuses to save; document the deviation in the README rather than half-implementing a second file format.
- COM deck scanning and slide thumbnails (§9.3/§9.5): the UI reads `--deck-info` JSON when given and otherwise validates without slide bounds.
- Multi-profile editing: v1 edits the effective profile only (what `ConfigToJson` writes).

---

### Task 1: `TargetEdit` and `ApplyEdit`

**Files:**
- Modify: `native/src/core/config_editor.h`, `native/src/core/config_editor.cpp`
- Test: `native/tests/unit/config_editor_test.cpp`

**Interfaces:**
- Consumes: `ConfigEditorModel` (existing `target_count()`, `rows()`, `SetSlide/SetHtml/...`).
- Produces:
  - `struct TargetEdit { int slide = 1; std::string label; std::string html; bool enabled = true; std::string assets_mode; };`
  - `bool ConfigEditorModel::ApplyEdit(std::size_t index, const TargetEdit& edit, std::string* error);` — validates slide ≥ 1 and html non-empty, writes all editable fields at once (one dirty transition), leaves `assets_mode` untouched when empty, and returns false with a message otherwise.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(config_editor_apply_edit_updates_all_fields_at_once) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  htmlppt::TargetEdit edit;
  edit.slide = 4;
  edit.label = "Live";
  edit.html = "page2.html";
  edit.enabled = false;
  std::string error;
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  const auto row = model.rows()[0];
  HTMLPPT_CHECK_EQ(row.slide, 4);
  HTMLPPT_CHECK_EQ(row.label, std::string{"Live"});
  HTMLPPT_CHECK_EQ(row.html, std::string{"page2.html"});
  HTMLPPT_CHECK(!row.enabled);
  HTMLPPT_CHECK(model.dirty());
}

HTMLPPT_TEST_CASE(config_editor_apply_edit_rejects_bad_input) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  htmlppt::TargetEdit edit;
  edit.slide = 0;
  std::string error;
  HTMLPPT_CHECK(!model.ApplyEdit(0, edit, &error));
  HTMLPPT_CHECK(!error.empty());
  edit.slide = 1;
  edit.html.clear();
  HTMLPPT_CHECK(!model.ApplyEdit(0, edit, &error));
  edit.html = "page2.html";
  HTMLPPT_CHECK(!model.ApplyEdit(5, edit, &error));
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
```

- [ ] **Step 2: Run it and watch it fail to compile**

Run: `cmake --build native/build --target htmlppt_core_tests`
Expected: `'TargetEdit' is not a member of 'htmlppt'`.

- [ ] **Step 3: Implement `TargetEdit` and `ApplyEdit` in `core/config_editor.{h,cpp}`**

`ApplyEdit` reuses `CheckIndex` and the existing field writes; it must not call `SetSlide`/`SetHtml` (one dirty transition, one error path).

- [ ] **Step 4: Run the tests and watch them pass**

Run: `native/build/htmlppt_core_tests.exe` → `[PASS] config_editor_apply_edit_*`, `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/config_editor.h native/src/core/config_editor.cpp native/tests/unit/config_editor_test.cpp
git commit -m "feat(core): apply a whole target edit atomically"
```

---

### Task 2: `SaveWithBackup`

**Files:**
- Modify: `native/src/core/config_editor.{h,cpp}`
- Test: `native/tests/unit/config_editor_test.cpp`

**Interfaces:**
- Produces: `bool ConfigEditorModel::SaveWithBackup(const std::filesystem::path& path, std::string* error);`
  - Writes `<path>.tmp`, copies the previous file to `<path>.bak` when it exists, replaces `<path>`, clears `dirty_`.
  - On any failure the original `<path>` is unchanged and `<path>.tmp` is removed.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(config_editor_save_with_backup_keeps_the_previous_file) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  const std::string first = ReadAll(path);
  // A CJK path must round-trip through the save.
  HTMLPPT_CHECK(model.SetLabel(0, std::string{"\xE5\xB1\x95\xE7\xA4\xBA"}, &error));
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(std::filesystem::exists(dir / "watcher.json.bak"));
  HTMLPPT_CHECK_EQ(ReadAll(dir / "watcher.json.bak"), first);
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "watcher.json.tmp"));
  HTMLPPT_CHECK(ReadAll(path).find("\xE5\xB1\x95\xE7\xA4\xBA") != std::string::npos);
}

HTMLPPT_TEST_CASE(config_editor_save_with_backup_leaves_the_file_on_failure) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  const std::string before = ReadAll(path);
  // A directory in place of the temp file makes the write fail.
  const auto blocker = dir / "watcher.json.tmp";
  std::filesystem::create_directories(blocker);
  HTMLPPT_CHECK(!model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(!error.empty());
  HTMLPPT_CHECK_EQ(ReadAll(path), before);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
```

`ReadAll(path)` is a file-local helper (`std::ifstream` + `istreambuf_iterator`) added next to `MakeDir` in the test file.

- [ ] **Step 2: Run and watch it fail**

Run: `cmake --build native/build --target htmlppt_core_tests`
Expected: `'SaveWithBackup' is not a member of 'htmlppt::ConfigEditorModel'`.

- [ ] **Step 3: Implement `SaveWithBackup`**

Write temp → copy old to `.bak` (if the old file exists) → `std::filesystem::remove(path)` → `rename(temp, path)`. On failure, `remove(temp, error_code)` and return false. `Save()` stays as the alias used by existing callers. `ConfigToJson` already emits `ordered_json`, so stable key order (spec §9.4) is inherited and needs no new work.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `native/build/htmlppt_core_tests.exe` → both new cases PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/config_editor.h native/src/core/config_editor.cpp native/tests/unit/config_editor_test.cpp
git commit -m "feat(core): save the config with a backup copy"
```

---

### Task 3: `DisplayRows` with per-row validation state

**Files:**
- Modify: `native/src/core/config_editor.{h,cpp}`
- Test: `native/tests/unit/config_editor_test.cpp`

**Interfaces:**
- Consumes: `ResolveTargets(config_, deck, options)` (existing), `HasErrorIssue`.
- Produces:
  - `struct DisplayRow { std::size_t index = 0; int slide = 0; std::string label; std::string html; std::string assets_mode; std::string state; std::string issue; };` where `state` is `"resolved"`, `"disabled"`, or `"error"`.
  - `std::vector<DisplayRow> ConfigEditorModel::DisplayRows(const DeckInfo* deck, ResolveOptions options) const;` — one row per target; `issue` names the first error whose path starts with `targets[<index>]`, empty otherwise; `state == "error"` when such an issue exists.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(config_editor_display_rows_mark_the_failing_target) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.AddTarget(1, "page2.html", &error));  // duplicate slide 1
  const auto deck = MakeDeck();
  const auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(rows[0].state, std::string{"error"});
  HTMLPPT_CHECK(rows[0].issue.find("duplicate") != std::string::npos);
  HTMLPPT_CHECK_EQ(rows[1].state, std::string{"error"});
}

HTMLPPT_TEST_CASE(config_editor_display_rows_keep_a_clean_config_clean) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto deck = MakeDeck();
  const auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(rows[0].state, std::string{"resolved"});
  HTMLPPT_CHECK(rows[0].issue.empty());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `'DisplayRows' is not a member of 'htmlppt::ConfigEditorModel'`.

- [ ] **Step 3: Implement `DisplayRow` / `DisplayRows`**

Reuse `rows()` for the fields, then attach the first issue whose `path` starts with `"targets[" + std::to_string(index) + "]"` and whose severity is Error; `disabled` comes from `target.enabled == false` and wins over `resolved` but not over `error`.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `native/build/htmlppt_core_tests.exe` → both new cases PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/config_editor.h native/src/core/config_editor.cpp native/tests/unit/config_editor_test.cpp
git commit -m "feat(core): derive a per-row validation view for the editor"
```

---

### Task 4: Window skeleton and `--config-ui`

**Files:**
- Create: `native/src/platform/win32/config_window.h`, `native/src/platform/win32/config_window.cpp`
- Create: `native/src/app/config_ui.h`, `native/src/app/config_ui.cpp`
- Modify: `native/src/app/cli.cpp` (command enum, parse, usage, dispatch), `native/CMakeLists.txt` (link `comctl32` on Windows)
- Test: `native/tests/integration/win32/config_window_test.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks (safe to build before the model work lands; it only creates a window).
- Produces:
  - `struct ConfigWindowOptions { std::wstring title; int width = 900; int height = 560; };`
  - `class ConfigWindow { public: bool Create(const ConfigWindowOptions& options); int RunMessageLoop(); bool Close(); HWND handle() const; };` — `Create` registers the class once, creates a hidden top-level window, shows it; `RunMessageLoop` returns the `WM_QUIT` code; `Close` posts `WM_CLOSE`. The window closes on `WM_CLOSE` unless the dirty prompt vetoes it (Task 8).
  - `int RunConfigUi(const std::filesystem::path& config_path, const std::filesystem::path& deck_info_path);` in `app/config_ui.h` (Windows-only; non-Windows stub in `supervisor.cpp` style returning 2 with `--config-ui requires Windows`).

- [ ] **Step 1: Write the failing integration test**

```cpp
#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <chrono>
#include <thread>

#include "platform/win32/config_window.h"

HTMLPPT_TEST_CASE(win32_config_window_creates_and_closes) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.title = L"htmlppt config";
  HTMLPPT_CHECK(window.Create(options));
  HTMLPPT_CHECK(window.handle() != nullptr);
  HTMLPPT_CHECK(IsWindow(window.handle()) != 0);

  window.Close();
  const DWORD deadline = GetTickCount() + 5000;
  while (GetTickCount() < deadline && IsWindow(window.handle()) != 0) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }
  HTMLPPT_CHECK(IsWindow(window.handle()) == 0);
}
#endif
```

- [ ] **Step 2: Run and watch it fail to compile**

Run: `cmake --build native/build --target htmlppt_win32_tests`
Expected: `config_window.h: No such file or directory`.

- [ ] **Step 3: Implement the window skeleton, the app entry, the CLI flag and `comctl32`**

- Window: `RegisterClassExW` once (a static `ATOM`), `CreateWindowExW(WS_EX_APPWINDOW, ..., WS_OVERLAPPEDWINDOW)`, `ShowWindow(SW_SHOW)`, `WM_DESTROY` → `PostQuitMessage(0)`.
- Title uses the config path when the caller does not override it.
- CLI: `--config-ui` command; `--config` supplies the file (required for the UI; a missing file is tolerated by Task 5, so the CLI must not reject an unreadable path — it only rejects an empty `--config`).
- CMake: `target_link_libraries(htmlppt_win32 PUBLIC comctl32)` and `InitCommonControlsEx(ICC_LISTVIEW_CLASSES)` in `Create`.

- [ ] **Step 4: Run the test and watch it pass**

Run: `ctest --test-dir native/build -R win32_tests --output-on-failure`
Expected: `[PASS] win32_config_window_creates_and_closes`, suite passes.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.h native/src/app/config_ui.cpp native/src/app/cli.cpp native/CMakeLists.txt native/tests/integration/win32/config_window_test.cpp
git commit -m "feat: open an empty config window from --config-ui"
```

---

### Task 5: Load the config and show the rows

**Files:**
- Modify: `native/src/app/config_ui.cpp`, `native/src/platform/win32/config_window.{h,cpp}`
- Test: `native/tests/unit/config_editor_test.cpp` (load-failure path stays model-side), plus the manual checklist entry in `docs/07-acceptance.md`

**Interfaces:**
- Consumes: `LoadConfigFile(path, overrides)` (existing, returns `ConfigLoadResult{config, issues}`), `ConfigEditorModel`, `DisplayRows`.
- Produces:
  - `ConfigWindow::SetRows(const std::vector<DisplayRow>& rows);` — clears and repopulates the `ListView`; called from the window thread only.
  - `RunConfigUi` loads the file; on failure it keeps an **empty model** and shows the first issue in the status line, then still opens the window (Review Focus 1).

- [ ] **Step 1: Write the failing test**

Model side only (the window itself is covered by Task 4's lifecycle test):

```cpp
HTMLPPT_TEST_CASE(config_editor_opens_an_empty_model_for_a_missing_file) {
  const auto result = htmlppt::LoadConfigFile("no-such-watcher.json");
  HTMLPPT_CHECK(!result.config.has_value());
  HTMLPPT_CHECK(!result.issues.empty());
  htmlppt::ConfigEditorModel model{htmlppt::Config{}};
  HTMLPPT_CHECK_EQ(model.target_count(), std::size_t{0});
  HTMLPPT_CHECK_EQ(model.DisplayRows(nullptr, htmlppt::ResolveOptions{}).size(),
                   std::size_t{0});
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: FAIL — `LoadConfigFile` returns a result whose `issues` is empty for a missing file, or the model cannot be built from a default `Config` (whichever the current code does; fix the loader's missing-file issue in this task if needed).

- [ ] **Step 3: Implement loading, the `ListView`, and the status line**

- `RunConfigUi`: `LoadConfigFile` → model → `DisplayRows` with a `DeckInfo` loaded from `deck_info_path` via `JsonDeckInfoProvider` when non-empty, else `nullptr` → `SetRows` → `RunMessageLoop`.
- `SetRows`: `LVS_REPORT | LVS_SINGLESEL`, columns `SLIDE / LABEL / HTML / ASSETS / STATE`; store the row index in `lParam`.
- Missing/bad config: status text `"cannot load <path>: <first issue>"`, model stays empty.

- [ ] **Step 4: Run the tests and the manual check**

Run: `native/build/htmlppt_core_tests.exe` → new case PASS; `native/build/htmlppt_win32_tests.exe` → suite PASS.
Manual: `htmlppt-watchdog --config-ui --config missing.json` opens a window with the error in the status line (record it in `docs/07-acceptance.md` item 20).

- [ ] **Step 5: Commit**

```bash
git add native/src/app/config_ui.cpp native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/tests/unit/config_editor_test.cpp docs/07-acceptance.md
git commit -m "feat: show the config targets in the editor window"
```

---

### Task 6: Edit a row through a modal dialog

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`
- Test: the dialog's field parsing is moved into a tested helper.

**Interfaces:**
- Consumes: `TargetEdit`, `ApplyEdit`, `SetRows`, `DisplayRows`.
- Produces:
  - `bool ParseTargetEditFields(const std::wstring& slide_text, const std::wstring& html, const std::wstring& label, TargetEdit* edit, std::wstring* error);` in `native/src/platform/win32/config_window.h` — header-only inline or in the .cpp with a declaration; the UI calls it before `ApplyEdit`, so bad input never reaches the model.
  - Double-click or the `Edit` button opens a dialog with `Slide` (edit), `Label` (edit), `HTML` (edit + `Browse…` via `GetOpenFileNameW`), `Enabled` (checkbox), `OK/Cancel`.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(win32_parse_target_edit_fields_rejects_non_numeric_slide) {
  htmlppt::TargetEdit edit;
  std::wstring error;
  HTMLPPT_CHECK(!htmlppt::ParseTargetEditFields(L"x", L"page1.html", L"", &edit, &error));
  HTMLPPT_CHECK(!error.empty());

  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(L"7", L"page1.html", L"Live", &edit, &error));
  HTMLPPT_CHECK_EQ(edit.slide, 7);
  HTMLPPT_CHECK_EQ(edit.html, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(edit.label, std::string{"Live"});

  // A CJK path must survive the wide-to-UTF-8 conversion.
  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(L"1", L"\u6f14\u793a/p.html", L"", &edit, &error));
  HTMLPPT_CHECK_EQ(edit.html, std::string{"\xE6\xBC\x94\xE7\xA4\xBA/p.html"});
}
```

Add it to `native/tests/integration/win32/config_window_test.cpp`.

- [ ] **Step 2: Run and watch it fail**

Expected: `'ParseTargetEditFields' is not a member of 'htmlppt'`.

- [ ] **Step 3: Implement the parser, the dialog and the edit flow**

`ParseTargetEditFields` uses `WideCharToMultiByte(CP_UTF8, ...)`; slide parsed with `std::from_chars`; empty slide text is an error. The dialog is a `DialogBoxW`-free plain window opened modally (`EnableWindow(parent, FALSE)`), or a `DialogBoxIndirectParamW` with an in-memory template — pick the simplest that works under MinGW and note the choice in a comment.

- [ ] **Step 4: Run the tests and the manual check**

Run: `native/build/htmlppt_win32_tests.exe` → new case PASS.
Manual: double-click a row, change slide/label, OK → list refreshes; bad slide → inline error, dialog stays open.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/tests/integration/win32/config_window_test.cpp
git commit -m "feat: edit a target row through a dialog"
```

---

### Task 7: Add, remove, validate and save

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`, `native/src/app/config_ui.cpp`
- Test: model-side coverage already exists (Tasks 1–3); the buttons' enablement rule is tested through `DisplayRows`.

**Interfaces:**
- Consumes: `AddTarget`, `RemoveTarget`, `SaveWithBackup`, `DisplayRows`, `HasErrorIssue`.
- Produces:
  - `bool ConfigEditorModel::HasBlockingError(const DeckInfo* deck, ResolveOptions options) const;` — true when any `DisplayRow.state == "error"`. The window's Save enablement mirrors this single rule.
  - `ConfigWindow::SetSaveEnabled(bool)` and `ConfigWindow::SetStatus(std::wstring)`.
  - `Save` is enabled only when `dirty()` **and** `!HasBlockingError(...)` (Review Focus 3); pressing it calls `SaveWithBackup` and reports success/failure in the status line.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(config_editor_blocks_save_while_a_row_has_an_error) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto deck = MakeDeck();
  std::string error;
  HTMLPPT_CHECK(!model.HasBlockingError(&deck, htmlppt::ResolveOptions{}));
  HTMLPPT_CHECK(model.AddTarget(1, "page2.html", &error));  // duplicate slide 1
  HTMLPPT_CHECK(model.HasBlockingError(&deck, htmlppt::ResolveOptions{}));
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
```

- [ ] **Step 2: Run and watch it fail**

Run: `cmake --build native/build --target htmlppt_core_tests`
Expected: `'HasBlockingError' is not a member of 'htmlppt::ConfigEditorModel'`.

- [ ] **Step 3: Implement `HasBlockingError` and the toolbar actions**

`HasBlockingError` iterates `DisplayRows` and returns true on the first `"error"`. `Add` opens the same dialog with slide = max(slide)+1 and html empty; `Remove` asks `MessageBoxW(MB_YESNO)` and removes the selected row; `Validate` recomputes `DisplayRows` and puts the first issue in the status line; `Save` follows the enablement rule above.

- [ ] **Step 4: Run the suites and the manual check**

Run: `ctest --test-dir native/build --output-on-failure` → 9/9.
Manual: add a duplicate → row shows error + Save disabled; fix it → Save enabled; save → `<path>.bak` appears.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp
git commit -m "feat: add, remove, validate and save from the editor"
```

---

### Task 8: Dirty prompt on close, DPI scaling, docs

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`, `native/src/app/cli.cpp` (usage), `native/README.md`, `docs/07-acceptance.md`
- Test: the prompt decision moves into a tested helper: `bool ShouldWarnBeforeClose(bool dirty, bool saved);` is trivial, so instead test the window's `WM_CLOSE` behaviour through the integration test with a dirty model.

**Interfaces:**
- Consumes: `dirty()`, `SaveWithBackup`.
- Produces: `ConfigWindow::SetDirty(bool)`; `WM_CLOSE` shows `MessageBoxW(MB_YESNOCANCEL)` when dirty (`Yes` saves then closes, `No` closes, `Cancel` keeps the window) — Review Focus 5.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(win32_config_window_keeps_open_when_close_is_cancelled) {
  // A dirty window must not be destroyed by WM_CLOSE without an answer. The test
  // drives the documented policy directly: ShouldCloseWindow(dirty=false) is
  // true; the interactive Yes/No/Cancel path is the manual checklist item.
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(false, false));
  HTMLPPT_CHECK(!htmlppt::ShouldCloseWindow(true, false));
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(true, true));
}
```

`bool ShouldCloseWindow(bool dirty, bool answer_save);` lives in `config_window.h` and is what `WM_CLOSE` consults; the dialog only supplies the answer.

- [ ] **Step 2: Run and watch it fail**

Expected: `'ShouldCloseWindow' is not a member of 'htmlppt'`.

- [ ] **Step 3: Implement the close policy, DPI scaling and the docs**

- `WM_CLOSE`: if `!dirty` or already saved → destroy; else prompt and take the answer.
- DPI: scale the initial size and the `ListView` row height by `GetDpiForWindow`; handle `WM_DPICHANGED` by resizing to the suggested rect.
- Docs: `--config-ui` in help + `native/README.md`; `docs/07-acceptance.md` items 20–23 (open missing config, edit/save, blocked save on error, close prompt).

- [ ] **Step 4: Run everything**

Run: `ctest --test-dir native/build --output-on-failure` (9/9), `node --test tests/*.test.mjs` (4/4), and `scripts/smoke-watchdog.ps1` once (the CLI change must not disturb a run).

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/cli.cpp native/README.md docs/07-acceptance.md
git commit -m "feat: finish the config editor's close policy and DPI scaling"
```
