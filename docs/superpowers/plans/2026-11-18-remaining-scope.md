# Remaining Scope Implementation Plan (backend wiring + §9 UI features)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the gaps left after Plan 5C and Config UI v2: wire journal reconciliation, mirror the legacy flag protocol, make per-target `focus`/`display`/`edge` overrides and `focus.require` effective, and add the remaining §9 editor features (undo/redo, dark theme, drag-drop, UI start/stop, hot reload, thumbnails, mapping wizard).

**Architecture:** Phase A is core/app wiring (testable without a desktop) and lands first; Phase B extends the existing `ConfigWindow`/`ConfigEditorModel` and is gated on Phase A's interfaces. Every task follows the repo's TDD + `HTMLPPT_TEST_CASE` conventions and ends with `ctest` 9/9 and `npm test` 4/4.

**Tech Stack:** C++20, Win32/Common Controls/DWM, existing core config/resolve/state-machine/worker-plan code, python-pptx only for the report deck (not here).

**Spec:** `docs/superpowers/specs/2026-11-14-cpp-watchdog-design.md` §6.2 (journal reconciliation), §8.2/§8.5 (per-target fields), §9.3 (wizard), §9.4 (undo/redo), §9.5 (dark theme, drag-drop, thumbnails), §9.6 (hot reload, UI start/stop), §14.1 (compat flags).

## Global Constraints

- `src/core/` is platform-neutral; Win32-only code lives in `src/platform/win32/` or behind `_WIN32` in `src/app/`.
- Clean under GCC 16 `-Wall -Wextra -Wpedantic -Werror`; Win32 targets also define `UNICODE`/`_UNICODE`/`NOMINMAX`.
- No new dependency; MinGW must keep compiling.
- The model/state machine stays the single owner of edits and transitions; views only forward.
- Every new behaviour gets a test; `ctest` and `node --test` stay green.
- Do not change `docs/superpowers/plans/**` or the superseded plan files.

## Review Focus

1. **Reconcile with no journal / a clean journal** — startup must be a no-op, not a spurious SafeMode.
2. **A stale legacy flag from a previous run** — ownership check decides delete vs leave; another run's flag is never removed.
3. **Per-target override absent vs present** — an unset override keeps the global default exactly.
4. **Hot reload while a switch/return is in flight** — the reload must be deferred, never applied mid-transition.
5. **Undo across a blocked save / after a wizard import** — history clear on import, and dirty/gate recomputed after undo.

## Phase A — backend wiring

### Task A1: Wire `ReconcileStartup` into supervisor startup

**Files:** `native/src/app/supervisor.cpp`, `native/tests/integration/win32/supervisor_host_test.cpp`
**Produces:** supervisor reads the state dir's journal tail before launching the worker, builds `ReconcileFacts` (edge alive via `ProcessIdsByCommandLineMarker(L"htmlppt-<run_id>")`, flags ownership via `CompatFlags::IsOwned(kLockFile)`, `overlay_owned=false`, `can_restore_wps=false`), journals `reconcile: <outcome> <reason>`, kills leftover Edge pids when `kill_edge`, and continues (a non-Clean outcome never blocks startup).
**Test:** pre-seed `journal.jsonl` with an unclean tail, start the supervisor, assert the journal gains a `reconcile:` line and the run still stops cleanly.
**Steps:** failing test → RED → implement → GREEN → `ctest` 9/9 → commit `feat(app): reconcile the previous run at supervisor startup`.

### Task A2: Mirror the legacy flag protocol

**Files:** `native/src/app/worker_session.{h,cpp}`, `native/src/app/supervisor.cpp`, `native/src/core/compat_flags.*` (only if a helper is missing), `native/tests/integration/win32/worker_session_test.cpp`
**Produces:** `WorkerSessionConfig::on_step` (called after every `driver.Step()` with the snapshot); `RunWorker` installs a lambda mapping state transitions to `CompatFlags::WriteFlag`: `cover-fade.flag` on entering CoverIn, `cover-ready.flag` on entering Live, `cover-hide.flag` on entering SafeMode, `cover-stop.flag` on entering Stopping, `agent-arm.flag` on entering Live (payload `edge_profile|exit=0|ready=1`), `agent-stop.flag` on entering Stopping, `edge-closed.flag` when the snapshot leaves Live for Monitoring after Live.
**Test:** a session test with a fake driver that drives the state sequence and asserts the callback fired with the expected snapshots; the flag writing is exercised through the same callback in a unit-style test using a temp dir.
**Steps:** RED → implement → GREEN → commit `feat: mirror the legacy flag protocol from the worker loop`.

### Task A3: Per-target `focus`/`display`/`edge` and `focus.require`

**Files:** `native/src/core/state_machine_types.h`, `native/src/core/worker_plan.{h,cpp}`, `native/src/core/worker_runtime.{h,cpp}`, `native/src/core/state_machine.cpp` (edge live timeout), `native/tests/unit/{worker_plan,state_machine,worker_runtime}_test.cpp`
**Produces:** `TargetPlan` gains `std::optional<Duration> edge_max_live`, `std::optional<Duration> focus_timeout`, `std::optional<FocusStrategy> focus_strategy`, `std::optional<ScreenMetrics> screen`, `bool focus_require = true`; `BuildWorkerPlan` resolves each from the target's optional config (falling back to defaults; `screen` comes from `SelectScreenMetrics` in the app layer — the plan field is filled by a new `ResolveTargetScreens` helper used by the supervisor); the machine's EdgeLive timeout uses `EffectiveEdgeMaxLive(target)`; the runtime applies the target's focus timing/strategy and screen when the trigger starts; `focus_require == false` skips focus attempts entirely.
**Tests:** absent override keeps the global value; present override changes it; `focus_require=false` performs no activation (fake window system records none); per-target screen replaces the default on trigger.
**Steps:** RED → implement → GREEN → commit `feat: honour per-target focus, display and edge overrides`.

## Phase B — §9 editor features

### Task B1: Undo/redo

**Files:** `native/src/core/config_editor.{h,cpp}`, `native/src/platform/win32/config_window.*`, `native/src/app/config_ui.cpp`, tests.
**Produces:** `ConfigEditorModel::Undo()/Redo()/CanUndo()/CanRedo()` over a bounded command stack of JSON snapshots (`ToJson()` before each mutation, cap 50); `Ctrl+Z`/`Ctrl+Y` in the window; dirty/gate recomputed after undo; history cleared on load and on a successful save.
**Test:** edit → undo restores the previous model; redo re-applies; undo past the start is a no-op; save clears the redo stack.

### Task B2: Dark theme

**Files:** `native/src/platform/win32/config_window.*`, tests.
**Produces:** `ConfigWindowOptions::dark_theme` (default follows the system `AppsUseLightTheme` registry value); dark backgrounds/foregrounds for the window, list, edits and status; `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` on the title bar; re-applied on `WM_DPICHANGED`/theme change.
**Test:** the option is honoured (window class brush + `DWMWA_USE_IMMERSIVE_DARK_MODE` query).

### Task B3: Drag-and-drop HTML onto a row

**Files:** `native/src/platform/win32/config_window.*`, `native/src/app/config_ui.cpp`, tests.
**Produces:** `DragAcceptFiles` on the window; `WM_DROPFILES` resolves the first file, fills the detail pane's HTML field, and (when a row is selected) applies the edit through the existing handler; a directory drop is rejected with a status message.
**Test:** a synthetic `WM_DROPFILES` (or the shared `ParseDroppedPath` helper) fills the HTML field; directory drop sets the status.

### Task B4: Start/stop the watchdog from the UI

**Files:** `native/src/app/config_ui.cpp`, `native/src/platform/win32/config_window.*`, tests.
**Produces:** toolbar buttons Start/Stop; Start launches `htmlppt-watchdog.exe --supervisor --config <path> --state-dir <dir> --run-id <id>` detached; Stop runs `--stop --run-id <id> --state-dir <dir>`; the status line shows the run id and the last `--status` state; Start is disabled while the single-instance mutex is held.
**Test:** the launch/stop argument builder is a pure function (`BuildSupervisorArgs`, `BuildStopArgs`) and is unit-tested; a focused integration test starts a supervisor for a fake config and stops it.

### Task B5: Hot reload while running

**Files:** `native/src/app/config_ui.cpp`, `native/src/platform/win32/config_window.*`, `native/src/core/config_editor.*`, tests.
**Produces:** a config-file watcher (`ReadDirectoryChangesW` on the config directory) that, when the editor is running a watchdog and the machine is in `Monitoring`, saves the model and lets the supervisor restart the worker into the new plan; while a switch/return is in flight the reload is deferred and the status says so. Reload only when `dirty` and no blocking error.
**Test:** the pure decision helper (`ShouldReload(state, dirty, blocked)`) is unit-tested; the watcher is manual.

### Task B6: Slide thumbnails

**Files:** `native/src/core/deck_info.*` (export request model), `native/src/app/config_ui.cpp`, `native/src/platform/win32/config_window.*`, tests.
**Produces:** a "Scan deck" button that uses `WpsHost` to export each slide's thumbnail PNG to the run temp dir; the list shows the thumbnails via `LVSIL_NORMAL`; failures fall back to title text and a status message. The COM export runs on a worker thread; the UI thread only receives results.
**Test:** the export request/result mapping and the fallback are unit-tested; the COM path is manual.

### Task B7: Auto-mapping wizard (§9.3)

**Files:** `native/src/core/mapping_wizard.{h,cpp}`, `native/src/platform/win32/wizard_window.*`, `native/src/app/config_ui.cpp`, tests.
**Produces:** a modal wizard: choose deck → scan slides (title/number) → choose an HTML root → propose pairs by `slide-NN.html` filename or by file order → preview table with per-row include → confirm writes targets into the model (one undoable command) → close.
**Test:** the pairing engine is pure and fully unit-tested (filename numbers, file order, manual skips, duplicate detection); the dialog is manual.
