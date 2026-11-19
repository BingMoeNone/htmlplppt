# Config UI v2 (Master-Detail Main Window) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the existing empty `--config-ui` window into the spec §9.2 core screen: a target list on the left, an editable detail pane on the right, a small toolbar (add / remove / save / validate), and a status line — saving through the tested model with a backup.

**Architecture:** The core model already owns every edit (`ApplyEdit`, `SaveWithBackup`, `DisplayRows`). This plan adds the view around it: `ConfigWindow` gains a right-hand detail pane bound to the selected row, and the window's controller functions (`SelectRow`, `ApplyDetail`, `RefreshRows`) are kept small and testable through accessors. A modal dialog is not used: the master-detail layout matches the spec and removes the dialog lifecycle from the test surface.

**Tech Stack:** C++20, Win32 + Common Controls v6 (`comctl32`), existing `ConfigEditorModel`, `HTMLPPT_TEST_CASE` suites.

**Spec:** `docs/superpowers/specs/2026-11-14-cpp-watchdog-design.md` §9.1/§9.2 (single profile, list + detail, toolbar, status bar) and §9.4 (validation, atomic save, backup). This plan **supersedes** `2026-11-17-cpp-watchdog-config-ui.md`.

## Already completed (reuse, do not redo)

| Commit | Deliverable | Evidence |
|---|---|---|
| `6921e10` | `TargetEdit` + `ConfigEditorModel::ApplyEdit` | 2 tests |
| `2d77b56` | `ConfigEditorModel::SaveWithBackup` (temp + `.bak`) | 2 tests |
| `0dc1979` | `DisplayRow` / `DisplayRows` | 2 tests |
| `c2cf524` | `ConfigWindow` skeleton, `--config-ui`, `comctl32`, `UNICODE` | window life-cycle test |

`git stash show -p "config-ui task5 wip"` holds a half-finished list population (`SetRows`, `SetStatus`, layout) from the previous plan's Task 5. **Reuse it** (it compiles green) rather than rewriting: Task 1 starts by applying that stash.

## Global Constraints

- `src/core/` stays platform-neutral (no `windows.h`); the view lives in `src/platform/win32/`.
- New code is clean under GCC 16 `-Wall -Wextra -Wpedantic -Werror`; the Win32 targets also compile with `UNICODE`/`_UNICODE` and `NOMINMAX` (already set).
- No new dependency; MinGW must keep compiling (`comctl32` is already linked).
- The model is the only owner of edits; the view never writes `Config` directly.
- Save stays atomic (temp + `.bak` + replace) and a failed save leaves the original intact.
- Every new behaviour gets a test in its task; `ctest` (9 suites) and `npm test` stay green.
- ASCII only in `scripts/*.ps1`.

## Review Focus

1. **No selection / selection cleared while rows change** — the detail pane must disable its Apply and never write to a stale index. (Task 2)
2. **Editing a row that already has a validation error** — the error must survive the refresh and the Save gate must stay closed. (Task 3)
3. **Save with no previous file vs with one** — `.bak` only when a previous file existed; a failed write leaves the original. (Task 4, already-tested model path)
4. **Closing with unsaved edits** — prompt, and Cancel keeps the window. (Task 5)
5. **DPI change while the pane is visible** — controls re-layout without overlapping or clipping the status line. (Task 6)

## File Structure

- `native/src/platform/win32/config_window.{h,cpp}` — window, list, detail pane, toolbar, status line, controller functions.
- `native/src/app/config_ui.cpp` — loads the config and deck info, builds the model, creates the window, sets rows/status (already exists; gains the model handle for Apply/Save).
- `native/src/core/config_editor.{h,cpp}` — gains `HasBlockingError` and `FromDisplayRow` helpers (tested).
- `native/tests/integration/win32/config_window_test.cpp` — view tests.
- `native/tests/unit/config_editor_test.cpp` — model tests.
- `docs/07-acceptance.md` — manual checklist additions.

## Out of scope (unchanged from v1)

Auto-mapping wizard (§9.3), undo/redo, drag-and-drop, thumbnails, dark/high-contrast theming, `IFileOpenDialog`/`IFileSaveDialog`, hot reload while running, UI-side start/stop, preview, profile/deck selectors, search/filter, deck fingerprint row, `.draft.json`, per-target `focus`/`display`/`edge` editing.

---

### Task 1: Land the parked list population

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`, `native/src/app/config_ui.cpp`, `native/CMakeLists.txt`, `native/tests/integration/win32/config_window_test.cpp` (all from the stash)
- Test: `native/tests/integration/win32/config_window_test.cpp`

**Interfaces:**
- Produces: `void ConfigWindow::SetRows(const std::vector<DisplayRow>& rows);`, `void ConfigWindow::SetStatus(const std::wstring& text);`, `HWND ConfigWindow::list_view() const;`
- Produces: `RunConfigUi` loads the config, builds `ConfigEditorModel`, computes `DisplayRows` (with `--deck-info` when given), populates the list and the status line; a missing config opens an empty model with the loader's first issue as status.

- [ ] **Step 1: Apply the stash without dropping it**

Run: `git stash apply stash@{0}` (the stash is `config-ui task5 wip`).
Expected: the 5 modified files return; `git status` shows them.

- [ ] **Step 2: Watch the view test fail against the HEAD implementation**

Run:
```bash
git checkout HEAD -- native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp native/CMakeLists.txt
cmake --build native/build --target htmlppt_win32_tests
```
Expected: compile errors naming `SetRows`/`SetStatus`/`list_view` — the test file (from the stash) is the RED state.

- [ ] **Step 3: Restore the parked implementation and run the test**

Run:
```bash
git checkout stash@{0} -- native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp native/CMakeLists.txt
cmake --build native/build --target htmlppt_win32_tests && native/build/htmlppt_win32_tests.exe
```
Expected: `[PASS] win32_config_window_shows_the_target_rows`, suite passes.

- [ ] **Step 4: Drop the stash and commit**

Run: `git stash drop stash@{0}` (the work is now in the tree), then:

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp native/CMakeLists.txt native/tests/integration/win32/config_window_test.cpp
git commit -m "feat: show the config targets in the editor window"
```

---

### Task 2: Detail pane and selection binding

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`
- Test: `native/tests/integration/win32/config_window_test.cpp`

**Interfaces:**
- Produces:
  - `struct DetailFields { std::wstring slide; std::wstring label; std::wstring html; std::wstring assets; bool enabled = true; };` (in `config_window.h`)
  - `void ConfigWindow::SelectRow(int row);` — selects the list row (clamped) and fills the detail controls.
  - `DetailFields ConfigWindow::detail_fields() const;` — reads the controls back (test seam).
  - `bool ConfigWindow::detail_enabled() const;` — false with no selection (all detail controls and Apply disabled).
  - `void ConfigWindow::ApplyDetail();` — validates the fields, calls `ConfigEditorModel::ApplyEdit` through a callback the app installs (see Task 3), refreshes the list.
- The right pane uses a `STATIC` label + `EDIT` for slide/label/html, `COMBOBOX` (dropdown list) for assets (`inherit`, `direct`, `file`, `tree`), and a `BUTTON` checkbox for enabled; `Apply` is disabled while there is no selection.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(win32_config_window_binds_selection_to_the_detail_pane) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].index = 0; rows[0].slide = 3; rows[0].label = "Live";
  rows[0].html = "page.html"; rows[0].assets_mode = "file";
  rows[0].state = "resolved";
  rows[1].index = 1; rows[1].slide = 5; rows[1].html = "two.html";
  rows[1].state = "resolved";
  window.SetRows(rows);

  window.SelectRow(0);
  auto fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"3"));
  HTMLPPT_CHECK_EQ(fields.html, std::wstring(L"page.html"));

  window.SelectRow(1);
  fields = window.detail_fields();
  HTMLPPT_CHECK_EQ(fields.slide, std::wstring(L"5"));
  // A cleared selection disables the pane instead of writing to a stale index.
  window.SelectRow(-1);
  HTMLPPT_CHECK(!window.detail_enabled());
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `'detail_fields' is not a member of 'htmlppt::ConfigWindow'`.

- [ ] **Step 3: Implement the pane, `SelectRow`, `detail_fields`, `detail_enabled`**

On `LVN_ITEMCHANGED` (via `WM_NOTIFY`) call the same `SelectRow(list index)` path; `SelectRow(-1)` disables the edits and the Apply button. Keep `std::vector<DisplayRow> rows_` in the window so the pane reads from the model's last snapshot, never from the list control's text.

- [ ] **Step 4: Run and watch it pass, then commit**

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/tests/integration/win32/config_window_test.cpp
git commit -m "feat: bind list selection to a detail pane"
```

---

### Task 3: Apply an edit from the detail pane

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`, `native/src/app/config_ui.cpp`
- Test: `native/tests/integration/win32/config_window_test.cpp` (parser), `native/tests/unit/config_editor_test.cpp` (model)

**Interfaces:**
- Consumes: `TargetEdit`, `ApplyEdit`, `DisplayRows`.
- Produces:
  - `bool ParseTargetEditFields(const std::wstring& slide_text, const std::wstring& html, const std::wstring& label, TargetEdit* edit, std::wstring* error);` in `config_window.h` — `slide` via `std::from_chars`, empty slide is an error, UTF-8 conversion for html/label, CJK paths survive.
  - `ConfigWindow::SetEditHandler(std::function<bool(const TargetEdit&, std::wstring* error)> handler);` — the window calls it on Apply; `RunConfigUi` installs a handler that runs `ParseTargetEditFields` → `ApplyEdit` → `DisplayRows` refresh → status.

- [ ] **Step 1: Write the failing parser test**

```cpp
HTMLPPT_TEST_CASE(win32_parse_target_edit_fields_rejects_non_numeric_slide) {
  htmlppt::TargetEdit edit;
  std::wstring error;
  HTMLPPT_CHECK(!htmlppt::ParseTargetEditFields(L"x", L"page1.html", L"", &edit, &error));
  HTMLPPT_CHECK(!error.empty());
  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(L"7", L"page1.html", L"Live", &edit, &error));
  HTMLPPT_CHECK_EQ(edit.slide, 7);
  HTMLPPT_CHECK_EQ(edit.label, std::string{"Live"});
  // CJK path survives wide -> UTF-8.
  HTMLPPT_CHECK(htmlppt::ParseTargetEditFields(L"1", L"\u6f14\u793a/p.html", L"", &edit, &error));
  HTMLPPT_CHECK_EQ(edit.html, std::string{"\xE6\xBC\x94\xE7\xA4\xBA/p.html"});
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `'ParseTargetEditFields' is not a member of 'htmlppt'`.

- [ ] **Step 3: Implement the parser and the Apply wiring**

`ParseTargetEditFields` uses `WideCharToMultiByte(CP_UTF8, ...)`; `ApplyDetail` calls the handler, and on success refreshes `rows_`, `SetRows`, and the status (`"applied"` or the handler's error). Also add `bool ConfigEditorModel::HasBlockingError(const DeckInfo* deck, ResolveOptions options) const;` with a model test first:

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

- [ ] **Step 4: Run both suites and the manual check**

Manual: open `demo/watcher.json`, select row 1, change label, Apply → the list row updates and the status shows applied; enter `x` as slide → status shows the parse error and the row is unchanged.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/config_editor.h native/src/core/config_editor.cpp native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp native/tests/unit/config_editor_test.cpp native/tests/integration/win32/config_window_test.cpp
git commit -m "feat: apply a detail edit through the model"
```

---

### Task 4: Toolbar actions (add, remove, save) and the Save gate

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`, `native/src/app/config_ui.cpp`
- Test: model-side (`HasBlockingError` already tested in Task 3); the toolbar's behaviour is exercised manually.

**Interfaces:**
- Produces: `ConfigWindow::SetSaveEnabled(bool)` and `bool ConfigWindow::save_enabled() const;`, `ConfigWindow::SetSaveHandler(std::function<bool(std::wstring* error)> handler);`, `ConfigWindow::SetAddHandler(...)`, `ConfigWindow::SetRemoveHandler(...)`; buttons send through the handlers and then refresh the rows/status.
- `RunConfigUi` installs handlers that call `AddTarget(max_slide + 1, "")` (rejected by the model, so offer a default `page.html`), `RemoveTarget(index)`, and `SaveWithBackup(config_path)`; Save is enabled only when `model.dirty() && !model.HasBlockingError(deck, options)`.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(win32_config_window_disables_save_while_a_row_is_an_error) {
  htmlppt::ConfigWindow window;
  htmlppt::ConfigWindowOptions options;
  options.show = false;
  HTMLPPT_CHECK(window.Create(options));
  window.SetSaveEnabled(false);
  HTMLPPT_CHECK(!window.save_enabled());
  window.SetSaveEnabled(true);
  HTMLPPT_CHECK(window.save_enabled());
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `'SetSaveEnabled' is not a member of 'htmlppt::ConfigWindow'`.

- [ ] **Step 3: Implement the toolbar and enablement**

Buttons `Add`, `Remove`, `Save`, `Validate` in a top row; `Validate` recomputes `DisplayRows` and puts the first issue in the status line; `Remove` asks `MessageBoxW(MB_YESNO)`; `Save` calls the handler and reports `saved` / the failure text.

- [ ] **Step 4: Run the suites and the manual check**

Manual: add → row with `page.html` and max+1 slide; duplicate it (edit slide to an existing one) → row shows error + Save disabled; delete the duplicate → Save enabled → Save → `demo/watcher.json.bak` appears.

- [ ] **Step 5: Commit**

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp
git commit -m "feat: add the editor toolbar and the save gate"
```

---

### Task 5: Dirty tracking, close prompt, and the DPI pass

**Files:**
- Modify: `native/src/platform/win32/config_window.{h,cpp}`, `native/src/app/config_ui.cpp`
- Test: `native/tests/integration/win32/config_window_test.cpp`

**Interfaces:**
- Produces: `bool ShouldCloseWindow(bool dirty, bool answer_save);` (header helper) and `ConfigWindow::SetDirty(bool)`; `WM_CLOSE` prompts with `MB_YESNOCANCEL` (`Yes` saves then closes, `No` closes, `Cancel` keeps the window). `WM_DPICHANGED` re-lays out to the suggested rect; initial sizes scale with `GetDpiForWindow`.

- [ ] **Step 1: Write the failing test**

```cpp
HTMLPPT_TEST_CASE(win32_should_close_window_honours_dirty_and_answer) {
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(false, false));
  HTMLPPT_CHECK(!htmlppt::ShouldCloseWindow(true, false));
  HTMLPPT_CHECK(htmlppt::ShouldCloseWindow(true, true));
}
```

- [ ] **Step 2: Run and watch it fail**

Expected: `'ShouldCloseWindow' is not a member of 'htmlppt'`.

- [ ] **Step 3: Implement the close policy and DPI layout**

`WM_CLOSE`: if `!dirty_` → destroy; else `MessageBoxW(MB_YESNOCALL)`; `IDYES` → save handler, destroy on success; `IDNO` → destroy; `IDCANCEL` → keep. `WM_DPICHANGED` → `SetWindowPos` to `lparam` rect, then `Layout()`.

- [ ] **Step 4: Run everything**

`ctest --test-dir native/build --output-on-failure` (9/9), `node --test tests/*.test.mjs` (4/4), `scripts/smoke-watchdog.ps1` once (the CLI change must not disturb a run).

- [ ] **Step 5: Docs and commit**

Add `--config-ui` to `native/README.md` with a screenshot-free walkthrough, and acceptance items 20–23 to `docs/07-acceptance.md` (open missing config, edit/save, blocked save, close prompt).

```bash
git add native/src/platform/win32/config_window.h native/src/platform/win32/config_window.cpp native/src/app/config_ui.cpp native/README.md docs/07-acceptance.md
git commit -m "feat: finish the config editor close policy and DPI layout"
```
