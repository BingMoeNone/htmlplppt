# C++ Watchdog Worker Loop (Plan 5C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `htmlppt-watchdog --supervisor/--worker` actually run the mode-B2 live flow: a worker polls WPS, feeds the state machine, and executes the cover/Edge/show/focus actions while heartbeating to the supervisor.

**Architecture:** A new platform-neutral `core/worker_runtime` owns the loop. It is handed five ports (`PresentationHost`, `CoverOverlay`, `EdgeLauncher`, `WindowSystem`, `Clock`), samples them into state-machine `Event`s, drains the emitted `Action`s back into port calls, and ticks deadlines. The Windows layer only assembles real ports. The supervisor resolves the config once and writes a `WorkerPlan` JSON into the state directory; the worker reads it, so resolution never runs twice.

**Tech Stack:** C++20, CMake + Ninja, nlohmann/json, existing adapters (`WpsHost`, `Win32CoverOverlay`, `Win32EdgeLauncher`, `Win32WindowSystem`, `CoverController`, `EdgeController`, `FocusController`).

**Spec:** `docs/superpowers/specs/2026-11-14-cpp-watchdog-design.md` (§4.3 worker threads, §4.5 data flow, §5.1–5.2 states/events, §7.5 event loop)

## Global Constraints

- `src/core/` stays platform-neutral: no `windows.h`, and it must build on Linux.
- The state machine is the only state owner. Ports report facts; only `StateMachine::Handle/Tick` decides transitions.
- No unbounded waits: every loop iteration sleeps at most `poll_interval`, and every controller call is deadline-bounded.
- New code must be clean under GCC 16 `-Wall -Wextra -Wpedantic -Werror` and MSVC `/W4 /permissive-`.
- No stubs or `TODO`s in shipped code; anything not implemented is not referenced.
- Timings come from `StateMachineConfig` and the config `defaults` (`cover.fadeMs/holdMs/maxHoldMs`, `edge.maxLiveMs`, focus interval/timeout). No magic numbers in the runtime.
- Tests use `HTMLPPT_TEST_CASE` / `HTMLPPT_CHECK`; fakes live under `native/tests/`, never in `src/`.
- Commit after every task with the message given in that task.

## Review Focus

The five failure modes the spec implies but that the state-machine tests cannot catch, each pinned to the task that owns its code:

1. **COM busy is not a show end.** `QueryShow()` returning `HostStatus::Busy` must emit `Failure(Com, Transient)` and keep `Monitoring`; mistaking it for `ShowEnded` would abort a live show. → Task 2.
2. **Edge dies before it is ready.** The process can exit between `Launch` and the first focus poll; that must produce `EdgeClosed` → cover hidden, WPS restored, never a stuck `Live`. → Task 4.
3. **Return title never arrives.** A page that never sets `TEF-LIVE-RETURN` must still resume via the `EdgeLive` timeout with the cover removed. → Task 6.
4. **Stop mid-trigger.** A stop while the cover is up must clean up exactly once, in the order cover-hide → Edge-kill → release, and exit 0. → Task 6.
5. **Supervisor heartbeat loss.** When the supervisor disappears the worker must not hang: it enters `DegradedNoSupervisor`, finishes or abandons the current step, and exits cleanly on stop. → Task 8.

---

## File Structure

Create:

- `native/src/core/worker_plan.h/.cpp` — `WorkerPlan` (machine view + resolved targets + deck path), config→plan mapping, JSON round-trip, and the plan file I/O. Responsible only for the supervisor→worker handoff.
- `native/src/core/worker_runtime.h/.cpp` — ports, host timing config, sample→event→action loop. The only place that knows how an `Action` maps to a port call.
- `native/tests/unit/worker_plan_test.cpp`
- `native/tests/unit/worker_runtime_test.cpp`
- `native/tests/unit/worker_fakes.h` — the four recording fakes and the `WorkerHarness` used by runtime tests.
- `scripts/smoke-watchdog.ps1` — real-machine smoke: start the supervisor, poll `--status`, stop, assert exit code.

Modify:

- `native/src/app/supervisor.h` — `SupervisorRunOptions` gains `config_path`.
- `native/src/app/supervisor.cpp` — supervisor resolves and writes the plan; worker runs the runtime loop.
- `native/src/app/cli.cpp` — `--supervisor` requires `--config`; keep `--worker --run-id --state-dir`.
- `native/README.md` — document the run flow and the smoke script.

Do not touch: `state_machine.*`, `resolve.*`, `config.*`, the Win32 adapters (already tested), or `scripts/*.py|.mjs|.vbs`.

---

### Task 1: WorkerPlan handoff and plan file

**Files:**
- Create: `native/src/core/worker_plan.h`, `native/src/core/worker_plan.cpp`
- Test: `native/tests/unit/worker_plan_test.cpp`

**Interfaces:**
- Consumes: `Config`, `ResolveResult`, `DeckInfo`, `RunPlan`, `TargetPlan`, `ResolvedTarget` (all existing).
- Produces:
  - `struct WorkerPlan { RunPlan machine; std::vector<ResolvedTarget> targets; std::string deck_utf8; };`
  - `WorkerPlan BuildWorkerPlan(const Config& config, const ResolveResult& resolved, int slide_count, std::string run_id);`
  - `std::string WorkerPlanToJson(const WorkerPlan& plan);`
  - `std::optional<WorkerPlan> WorkerPlanFromJson(std::string_view json, std::string* error);`
  - `bool WriteWorkerPlanFile(const WorkerPlan& plan, const std::filesystem::path& path, std::string* error);`
  - `std::optional<WorkerPlan> ReadWorkerPlanFile(const std::filesystem::path& path, std::string* error);`

JSON keys (fixed, camelCase): `runId`, `deck`, `machine { slideCount, targets[] }` where a machine target is `{slide, label, edgeProfile, trigger, onFailure, maxConsecutiveTriggerFailures, coverMaxHoldMs, enabled}`, and `targets[]` where a resolved target is `{ordinal, slide, label, htmlPath, query, fragment, stageName, edgeProfile}`. Unknown keys are ignored on read; a missing `runId` or a `machine.targets`/`targets` length mismatch is an error.

- [ ] **Step 1: Write the failing mapping test**

```cpp
HTMLPPT_TEST_CASE(worker_plan_maps_resolved_targets_to_machine_targets) {
  Config config = LoadTestConfig();          // 2 targets: slide 5 enabled, slide 8 disabled
  ResolveResult resolved = ResolveTestTargets(config);
  const WorkerPlan plan = BuildWorkerPlan(config, resolved, 12, "run-1");
  HTMLPPT_CHECK_EQ(plan.machine.run_id, std::string{"run-1"});
  HTMLPPT_CHECK_EQ(plan.machine.slide_count, 12);
  HTMLPPT_CHECK_EQ(plan.machine.targets.size(), std::size_t{1});   // disabled dropped
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].slide, 5);
  HTMLPPT_CHECK_EQ(plan.machine.targets[0].edge_profile,
                   resolved.targets[0].edge_profile);
  HTMLPPT_CHECK_EQ(plan.targets.size(), std::size_t{1});           // specs stay in step
  HTMLPPT_CHECK_EQ(plan.deck_utf8, Utf8Path(ResolveDeckPath(config)));
}
```

- [ ] **Step 2: Run it and watch it fail to compile**

Run: `cmake --build native/build --target htmlppt_core_tests`
Expected: FAIL — `BuildWorkerPlan` undeclared.

- [ ] **Step 3: Implement `BuildWorkerPlan`**

Build one `TargetPlan` per resolved target with `slide > 0` and `enabled`, copying `label`, `edge_profile`, effective `trigger`/`on_failure` (per-target value else `config.defaults`), `max_consecutive_trigger_failures`, and `cover_max_hold` from the per-target `cover` override. Sort both vectors by `slide` and filter identically so index `i` always addresses the same slide.

- [ ] **Step 4: Run the test and watch it pass**

Run: `cmake --build native/build --target htmlppt_core_tests && native/build/htmlppt_core_tests`
Expected: PASS.

- [ ] **Step 5: Write the failing round-trip and file tests**

```cpp
HTMLPPT_TEST_CASE(worker_plan_json_round_trip_preserves_every_field) {
  const WorkerPlan in = MakePlanWithEverything();   // CJK label + CJK html path
  std::string error;
  const auto out = WorkerPlanFromJson(WorkerPlanToJson(in), &error);
  HTMLPPT_CHECK(out.has_value());
  HTMLPPT_CHECK_EQ(out->machine.targets[0].label, in.machine.targets[0].label);
  HTMLPPT_CHECK_EQ(out->targets[0].html_path, in.targets[0].html_path);
  HTMLPPT_CHECK_EQ(out->deck_utf8, in.deck_utf8);
}
HTMLPPT_TEST_CASE(worker_plan_json_rejects_missing_run_id) {
  std::string error;
  HTMLPPT_CHECK(!WorkerPlanFromJson(R"({"machine":{"slideCount":3}})", &error).has_value());
  HTMLPPT_CHECK(!error.empty());
}
HTMLPPT_TEST_CASE(worker_plan_file_round_trip_uses_utf8) {
  const auto dir = TempDir();
  const WorkerPlan in = MakePlanWithEverything();
  std::string error;
  HTMLPPT_CHECK(WriteWorkerPlanFile(in, dir / "worker-plan.json", &error));
  const auto out = ReadWorkerPlanFile(dir / "worker-plan.json", &error);
  HTMLPPT_CHECK(out.has_value());
  HTMLPPT_CHECK_EQ(out->targets[0].html_path, in.targets[0].html_path);
}
```

- [ ] **Step 6: Run it and watch it fail**

Run: `cmake --build native/build --target htmlppt_core_tests && native/build/htmlppt_core_tests`
Expected: FAIL — `WorkerPlanToJson` undeclared.

- [ ] **Step 7: Implement the JSON round-trip and file I/O**

Serialize UTF-8 strings as-is. `WorkerPlanFromJson` validates non-empty `runId`, `slideCount >= 0`, and equal `machine.targets`/`targets` lengths; on any failure set `*error` and return `nullopt`. File I/O writes UTF-8 (and UTF-8 BOM-free) and reports `std::error_code` messages into `*error`; a missing file is `nullopt` with a message, not an exception.

- [ ] **Step 8: Run all three tests and the core suite**

Run: `ctest --test-dir native/build -R core_tests --output-on-failure`
Expected: PASS, 0 failures.

- [ ] **Step 9: Commit**

```bash
git add native/src/core/worker_plan.h native/src/core/worker_plan.cpp native/tests/unit/worker_plan_test.cpp
git commit -m "feat(core): add worker plan handoff with json and file round-trip"
```

---

### Task 2: Runtime skeleton and show sampling

**Files:**
- Create: `native/src/core/worker_runtime.h`, `native/src/core/worker_runtime.cpp`, `native/tests/unit/worker_fakes.h`
- Test: `native/tests/unit/worker_runtime_test.cpp`

**Interfaces:**
- Consumes: `StateMachine`, `WorkerPlan`, `PresentationHost`, `CoverOverlay`, `EdgeLauncher`, `WindowSystem`, `Clock`, `CoverController`, `EdgeController`, `FocusController`.
- Produces:
  - `struct WorkerPorts { PresentationHost& presentation; CoverOverlay& overlay; EdgeLauncher& launcher; WindowSystem& windows; Clock& clock; };`
  - `struct WorkerHostConfig { std::filesystem::path temp_dir; std::filesystem::path edge_exe; EdgeConfig edge; CoverTiming cover; FocusTiming focus; ScreenMetrics screen; std::vector<std::uint32_t> wps_pids; Duration poll_interval{100}; };`
  - `class WorkerRuntime { public: WorkerRuntime(WorkerPlan, StateMachineConfig, WorkerPorts, WorkerHostConfig); void Start(); bool Step(); bool done() const; void RequestStop(std::string reason); const StateSnapshot& snapshot() const; const std::vector<std::string>& journal() const; };`
  - `worker_fakes.h` also provides `WorkerHarness` (owns fakes, `RuntimeClock`, plan, config, runtime; `Start()`, `Step()`, `advance(Duration)`) and the builders `HTriggeredAtSlide1()`, `HLiveAtSlide1()` used by Tasks 3–6. A helper never consumes a phase the test it feeds is about to assert: `HTriggeredAtSlide1()` stops at `CoverIn`.

- [ ] **Step 1: Write the failing show-sampling test**

```cpp
HTMLPPT_TEST_CASE(worker_runtime_starts_show_and_tracks_slide_changes) {
  WorkerHarness h;                       // fakes from worker_fakes.h
  h.presentation.show = {HostStatus::Ok, true, 1};
  h.Start();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Warming);   // Start only
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  h.presentation.show.slide = 2;
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().prev_slide, 2);
}
HTMLPPT_TEST_CASE(worker_runtime_reports_com_busy_without_show_ended) {
  WorkerHarness h;
  h.presentation.show = {HostStatus::Ok, true, 1};
  h.Start(); h.runtime.Step();
  h.presentation.show.status = HostStatus::Busy;
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
  HTMLPPT_CHECK(h.presentation.exit_show_calls == 0);
}
```

- [ ] **Step 2: Run and watch it fail**

Run: `cmake --build native/build --target htmlppt_core_tests`
Expected: FAIL — `WorkerRuntime` undeclared.

- [ ] **Step 3: Implement ports, host config, and `Start`/`Step`**

`Start()` calls `machine_.Start()` and dispatches the returned actions. `Step()` runs, in order: `ObserveShow()`, `ObserveCover()`, `ObserveEdge()`, `machine_.Tick()`, dispatch, then sleep; returns `!done_`. `ObserveShow()` keeps `last_show_`, emits `ShowStarted` on `!active → active`, `ShowEnded` on `active → !active`, `SlideChanged(old, new)` when `slide != last_slide_ && slide > 0`, and on `status == Busy` emits `MakeFailure(Transient, Com, detail)` **without** updating `last_show_`. `done_` is set when the machine state is `Exited` or `SafeMode`.

- [ ] **Step 4: Run and watch both pass**

Run: `ctest --test-dir native/build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/worker_runtime.h native/src/core/worker_runtime.cpp native/tests/unit/worker_runtime_test.cpp native/tests/unit/worker_fakes.h
git commit -m "feat(core): add worker runtime show sampling"
```

---

### Task 3: Cover execution and hold/done events

**Files:**
- Modify: `native/src/core/worker_runtime.cpp` (`Dispatch`/`Execute`/`ObserveCover`)
- Test: `native/tests/unit/worker_runtime_test.cpp`

**Interfaces:**
- Consumes: `CoverController::{FadeIn,FadeOut,Hide,Tick}`, `CoverOverlay`, `CoverTick{opacity, hold_reached, done}`.
- Produces: `CoverFadeIn → FadeIn()`, `CoverFadeOut → FadeOut()`, `CoverHide → Hide()`; each `Step()` ticks the controller and emits `MakeCoverHold` on a new `hold_reached`, `MakeCoverDone` on a new `done` (edge-triggered, not every tick).

- [ ] **Step 1: Write the failing cover test**

```cpp
HTMLPPT_TEST_CASE(worker_runtime_fades_cover_in_holds_and_reports_done) {
  WorkerHarness h;
  h.config.startup_ignore = Duration{0};
  h.plan.machine.targets = {{1, "t1", "p1"}};      // target slide 1
  h.rebuild();                                     // re-create machine with the new plan
  h.presentation.show = {HostStatus::Ok, true, 1};
  h.Start(); h.runtime.Step();
  h.presentation.show.slide = 2;                   // leave-forward
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.overlay.show_calls, 1);       // CoverFadeIn ran
  HTMLPPT_CHECK(h.overlay.opacity > 0.0);
  // CoverHold fires only after fade + hold, never after hold alone.
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);    // CoverHold unblocked EdgeStarting
}
```

- [ ] **Step 2: Run and watch it fail**

Run: `cmake --build native/build --target htmlppt_core_tests && native/build/htmlppt_core_tests`
Expected: FAIL — `overlay.show_calls == 0`.

- [ ] **Step 3: Implement the action dispatch and cover tick**

Add `Dispatch(const std::vector<Action>&)` and `Execute(const Action&)` with a `switch` over `ActionKind`. Kinds owned by later tasks (`LaunchEdge`, `KillEdge`, `FocusEdge`, `ExitShow`, `ResumeShow`, `RestoreShow`, `FocusWps`) only append to `journal_` until their task lands. `ObserveCover()` ticks the controller (which applies opacity to the overlay) and emits `CoverHold`/`CoverDone` on phase edges.

- [ ] **Step 4: Run and watch it pass**

Run: `ctest --test-dir native/build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/worker_runtime.cpp native/tests/unit/worker_runtime_test.cpp
git commit -m "feat(core): execute cover actions and emit hold/done"
```

---

### Task 4: Edge launch, readiness, closure and return title

**Files:**
- Modify: `native/src/core/worker_runtime.cpp` (`ObserveEdge`, `Execute`)
- Test: `native/tests/unit/worker_runtime_test.cpp`

**Interfaces:**
- Consumes: `EdgeController`, `MakeEdgeLaunchSpec(edge_exe, temp_dir, ResolvedTarget, EdgeConfig)`, `SelectEdgeWindow`, `IsReturnTitle`, `FocusController::FocusEdge`.
- Produces: `LaunchEdge → edge_.Launch(spec)` for the action's slide; `KillEdge → edge_.Kill()`; observation emits `MakeEdgeReady` once after a successful `FocusEdge`, `MakeEdgeClosed` when a launched process is no longer alive, `MakeReturnRequested` once per trigger when the window title matches `kReturnTitleMarker`.

- [ ] **Step 1: Write the failing edge tests**

```cpp
// HTriggeredAtSlide1(): target slide 1, show active at slide 1, Start + Step,
// then slide -> 2 and one more Step. Leaves the machine in CoverIn with the
// cover fading in (no CoverHold consumed yet).
HTMLPPT_TEST_CASE(worker_runtime_launches_edge_then_reports_ready) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // CoverHold -> EdgeStarting + LaunchEdge
  HTMLPPT_CHECK_EQ(h.launcher.launch_count, 1);
  HTMLPPT_CHECK_EQ(h.launcher.last_spec.profile_dir, h.host.temp_dir / "p1");
  h.windows.windows = {WindowInfo{.id = 1, .pid = h.launcher.pid, .title = "TEF-LIVE-STAGE"}};
  h.runtime.Step();                             // EdgeReady -> ShowExit
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ShowExit);
}
HTMLPPT_TEST_CASE(worker_runtime_treats_dead_edge_as_closed_before_ready) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                             // Edge launched, waiting for ready
  h.launcher.set_alive(false);
  h.runtime.Step();                             // EdgeClosed -> switch failure
  HTMLPPT_CHECK_EQ(h.overlay.hide_calls, 1);    // switch failure hides the cover
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Monitoring);
}
```

- [ ] **Step 2: Run and watch them fail**

Run: `cmake --build native/build --target htmlppt_core_tests && native/build/htmlppt_core_tests`
Expected: FAIL — `launch_count == 0`.

- [ ] **Step 3: Implement edge observation**

Resolve the `ResolvedTarget` for `action.slide` from `plan_.targets`, build with `MakeEdgeLaunchSpec`, store `edge_pid_`, reset `ready_sent_`/`return_sent_`. In `ObserveEdge()` (only when `edge_pid_ != 0`): if `!edge_.active()` emit `EdgeClosed` and clear the pid; otherwise `SelectEdgeWindow(windows_.Enumerate(), edge_.pid())`; on a return title with `!return_sent_` emit `ReturnRequested`; else if `!ready_sent_` and `focus_.FocusEdge(pid)` succeeds, emit `EdgeReady`.

- [ ] **Step 4: Run and watch all runtime tests pass**

Run: `ctest --test-dir native/build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/worker_runtime.cpp native/tests/unit/worker_runtime_test.cpp
git commit -m "feat(core): launch and observe edge from the worker loop"
```

---

### Task 5: Show and focus actions

**Files:**
- Modify: `native/src/core/worker_runtime.cpp` (`Execute`)
- Test: `native/tests/unit/worker_runtime_test.cpp`

**Interfaces:**
- Consumes: `PresentationHost::{ExitShow,ResumeShow,FocusShow}`, `FocusController::FocusWps`, `SelectWpsShowWindow`, `WorkerHostConfig::{screen,wps_pids}`.
- Produces: `ExitShow → presentation_.ExitShow()`; `ResumeShow → presentation_.ResumeShow(slide)`; `RestoreShow → presentation_.ResumeShow(slide)` (that call already ensures the show runs, focuses and jumps); `FocusWps → focus_.FocusWps(host_.wps_pids, host_.screen)`; `FocusEdge → focus_.FocusEdge(edge_.pid())`.

- [ ] **Step 1: Write the failing show-action test**

```cpp
HTMLPPT_TEST_CASE(worker_runtime_exits_show_then_resumes_on_return) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                            // EdgeStarting + LaunchEdge
  h.windows.windows = {WindowInfo{.id = 1, .pid = h.launcher.pid, .title = "TEF-LIVE-STAGE"}};
  h.runtime.Step();                            // EdgeReady -> ShowExit + ExitShow
  HTMLPPT_CHECK_EQ(h.presentation.exit_show_calls, 1);
  h.presentation.show = {HostStatus::Ok, false, 0};   // show ended
  h.runtime.Step();                            // ShowEnded -> Live
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Live);
  h.windows.windows = {WindowInfo{.id = 2, .pid = h.launcher.pid, .title = "TEF-LIVE-RETURN"}};
  h.runtime.Step();                            // ReturnRequested -> Returning
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                            // CoverHold -> ReturnCoverOut + ResumeShow
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ReturnCoverOut);
  HTMLPPT_CHECK_EQ(h.presentation.resume_slides.back(), 1);
}
```

- [ ] **Step 2: Run and watch it fail**

Run: `cmake --build native/build --target htmlppt_core_tests && native/build/htmlppt_core_tests`
Expected: FAIL — `exit_show_calls == 0`.

- [ ] **Step 3: Implement the remaining action kinds**

Fill the `switch` cases. `FocusWps` passes `host_.wps_pids`; the app layer fills them after `Launch()` by enumerating windows and keeping the pids whose class/title belongs to WPS (the app already has `Win32WindowSystem`). Core never calls `PrimaryScreenMetrics()`.

- [ ] **Step 4: Run and watch all pass**

Run: `ctest --test-dir native/build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/worker_runtime.cpp native/tests/unit/worker_runtime_test.cpp
git commit -m "feat(core): execute show and focus actions"
```

---

### Task 6: Loop driver, stop, cleanup and the live timeout

**Files:**
- Modify: `native/src/core/worker_runtime.cpp` (`Step`, `RequestStop`)
- Test: `native/tests/unit/worker_runtime_test.cpp`

**Interfaces:**
- Consumes: `StateMachine::NextDeadline()`, `Clock::sleep_until`, `State::{Stopping,Exited,SafeMode}`, `TimeoutKind::EdgeLive`.
- Produces: `Step()` sleeps until `min(now + poll_interval, NextDeadline())`; `RequestStop(reason)` injects `MakeStopRequested` on the next `Step`; `done()` is true once the machine is `Exited`/`SafeMode`; cleanup actions (`CoverHide`, `KillEdge`, `ReleaseLock`, `StopHelperThreads`) run at most once, in the machine's order.

- [ ] **Step 1: Write the failing stop and timeout tests**

```cpp
HTMLPPT_TEST_CASE(worker_runtime_stop_mid_trigger_cleans_up_once) {
  WorkerHarness h = HTriggeredAtSlide1();
  h.runtime.RequestStop("test");
  h.runtime.Step();
  HTMLPPT_CHECK(h.overlay.hide_calls >= 1);
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 1);
  while (h.runtime.Step()) h.advance(Duration{10});
  HTMLPPT_CHECK(h.runtime.done());
  h.runtime.Step();                          // no further cleanup
  HTMLPPT_CHECK_EQ(h.launcher.kill_calls, 1);
}
HTMLPPT_TEST_CASE(worker_runtime_step_sleeps_at_most_poll_interval) {
  WorkerHarness h;
  const ClockPoint before = h.clock.now();
  h.runtime.Step();
  HTMLPPT_CHECK(h.clock.sleeps().back() - before <= h.host.poll_interval + Duration{1});
}
HTMLPPT_TEST_CASE(worker_runtime_edge_live_timeout_removes_cover_and_resumes) {
  WorkerHarness h = HLiveAtSlide1();         // Live, Edge up, no return title ever arrives
  h.advance(h.config.edge_max_live + Duration{1});
  h.runtime.Step();                          // EdgeLive timeout -> Returning (cover fades in)
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::Returning);
  h.advance(h.host.cover.fade + h.host.cover.hold + Duration{1});
  h.runtime.Step();                          // CoverHold -> ReturnCoverOut + ResumeShow
  HTMLPPT_CHECK_EQ(h.runtime.snapshot().state, State::ReturnCoverOut);
  HTMLPPT_CHECK_EQ(h.overlay.hide_calls, 0); // fade out, not a failure hide
  HTMLPPT_CHECK(h.presentation.resume_slides.back() == 1);
}
```

- [ ] **Step 2: Run and watch them fail**

Run: `cmake --build native/build --target htmlppt_core_tests && native/build/htmlppt_core_tests`
Expected: FAIL — `done()` never true / stop ignored.

- [ ] **Step 3: Implement the driver**

Compute the wake time before dispatch: `auto wake = now + host_.poll_interval; if (auto d = machine_.NextDeadline(); d && *d < wake) wake = *d; clock_.sleep_until(wake);`. Once `done_` is set, `Step()` returns false and executes nothing. Track `cleanup_done_` so a repeated stop cannot re-run cleanup.

- [ ] **Step 4: Run and watch all pass**

Run: `ctest --test-dir native/build -R core_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/worker_runtime.cpp native/tests/unit/worker_runtime_test.cpp
git commit -m "feat(core): drive the worker loop with bounded sleeps and one-shot cleanup"
```

---

### Task 7: Supervisor side — require a config and write the plan

**Files:**
- Modify: `native/src/app/supervisor.h`, `native/src/app/supervisor.cpp`, `native/src/app/cli.cpp`, `native/tests/cmake/cli_contract.cmake`

**Interfaces:**
- Consumes: `LoadConfigFile`, `ResolveTargets`, `WpsHost::Load`/`JsonDeckInfoProvider`, `BuildWorkerPlan`, `WriteWorkerPlanFile`.
- Produces: `SupervisorRunOptions::config_path`; `--supervisor` without `--config` exits 2 with `--config is required for --supervisor`; before launching the worker the supervisor writes `<state_dir>/worker-plan.json` and journal-logs `plan`.

- [ ] **Step 1: Write the failing CLI test**

Append to `native/tests/cmake/cli_contract.cmake`:

```cmake
execute_process(
  COMMAND "${EXE}" --supervisor
  ERROR_VARIABLE supervisor_err
  RESULT_VARIABLE supervisor_rc)
if (NOT supervisor_rc EQUAL 2)
  message(FATAL_ERROR "--supervisor without --config: rc=${supervisor_rc}")
endif()
if (NOT supervisor_err MATCHES "config is required")
  message(FATAL_ERROR "--supervisor without --config: ${supervisor_err}")
endif()
```

- [ ] **Step 2: Run and watch it fail**

Run: `ctest --test-dir native/build -R cli_contract --output-on-failure`
Expected: FAIL — exit code is not 2.

- [ ] **Step 3: Require the config**

In `cli.cpp`, treat `--supervisor` like the other `needs_config` commands (message `--config is required for --supervisor`), store it in `SupervisorRunOptions`, and keep `--worker` unchanged.

- [ ] **Step 4: Run and watch it pass**

Run: `ctest --test-dir native/build -R cli_contract --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Write the plan from the supervisor**

In `RunSupervisor`, before launching the worker: `LoadConfigFile(config_path)`, obtain `DeckInfo` (`WpsHost::Load(ResolveDeckPath(config))`, or the CLI `--deck-info` JSON when provided), `ResolveTargets`, `BuildWorkerPlan(config, resolved, deck.slide_count, run_id)`, `WriteWorkerPlanFile(plan, state_dir / "worker-plan.json", &error)`. On any failure: journal `plan failed`, print the error, exit 1. Also factor the `Config` → `StateMachineConfig` + `WorkerHostConfig` mapping into `worker_plan.h` so supervisor and worker cannot drift.

- [ ] **Step 6: Run the native suite**

Run: `cmake --build native/build --parallel && ctest --test-dir native/build --output-on-failure`
Expected: 100% passed.

- [ ] **Step 7: Commit**

```bash
git add native/src/app/supervisor.h native/src/app/supervisor.cpp native/src/app/cli.cpp native/src/core/worker_plan.h native/src/core/worker_plan.cpp native/tests/cmake/cli_contract.cmake
git commit -m "feat(app): require a supervisor config and write the worker plan"
```

---

### Task 8: Worker side — run the loop with real adapters

**Files:**
- Modify: `native/src/app/supervisor.cpp` (`RunWorker`)
- Test: `native/tests/integration/win32/supervisor_host_test.cpp` (extend the existing worker case)

**Interfaces:**
- Consumes: `ReadWorkerPlanFile`, `WorkerRuntime`, `WpsHost`, `Win32CoverOverlay`, `Win32EdgeLauncher`, `Win32WindowSystem`, `FindEdgeExecutable`, `SteadyClock`, the existing pipe/blackbox code.
- Produces: `--worker` runs `runtime.Start()` / `runtime.Step()` until done; sends a heartbeat frame every `heartbeat_interval` carrying `snapshot().state`; on `FrameType::Stop` calls `RequestStop`; on a broken pipe feeds `MakeSupervisorLost` and continues per `SupervisorLossPolicy::Continue`; exits 0 after the final `Cleanup` frame.

- [ ] **Step 1: Write the failing supervisor-loss test**

```cpp
HTMLPPT_TEST_CASE(worker_survives_supervisor_loss_and_exits_cleanly) {
  const auto dir = TempDir();
  WriteWorkerPlanFile(MinimalPlan(), dir / "worker-plan.json", nullptr);
  // Start the worker with a pipe that is closed immediately.
  const int rc = RunWorkerForTest(dir, /*close_pipe_after_ms=*/200);
  HTMLPPT_CHECK_EQ(rc, 0);
  HTMLPPT_CHECK(ReadJournalText(dir).find("supervisor lost") != std::string::npos);
}
```

- [ ] **Step 2: Run and watch it fail**

Run: `cmake --build native/build --target htmlppt_win32_tests && native/build/htmlppt_win32_tests`
Expected: FAIL — the worker returns 1 on a broken pipe today.

- [ ] **Step 3: Replace the heartbeat stub with the runtime**

Read and parse `worker-plan.json` (exit 1 with a clear message when absent or invalid). Assemble the real ports, construct `WorkerRuntime`, and loop: receive with a 0-ms timeout, translate `Stop` → `RequestStop`, `Step()`, heartbeat when due. A `broken()` client feeds `MakeSupervisorLost("heartbeat pipe closed")` if the policy is `Continue` and keeps running; the runtime reaches `Stopping` on stop. Always `Release()` the `WpsHost` and leave the overlay hidden in a `finally`-style scope guard.

- [ ] **Step 4: Run and watch it pass**

Run: `ctest --test-dir native/build -R win32_tests --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Run the full native suite**

Run: `ctest --test-dir native/build --output-on-failure`
Expected: 100% passed.

- [ ] **Step 6: Commit**

```bash
git add native/src/app/supervisor.cpp native/tests/integration/win32/supervisor_host_test.cpp
git commit -m "feat(app): run the worker loop with real win32 adapters"
```

---

### Task 9: Real-machine smoke script

**Files:**
- Create: `scripts/smoke-watchdog.ps1`
- Modify: `native/README.md`

**Interfaces:**
- Consumes: `htmlppt-watchdog.exe`, a config JSON, a state directory, `--status`, `--stop`.
- Produces: a script that exits 0 only when the supervisor started, reported a state, and stopped cleanly.

- [ ] **Step 1: Write the script**

```powershell
param(
  [Parameter(Mandatory)] [string]$Config,
  [Parameter(Mandatory)] [string]$StateDir,
  [Parameter(Mandatory)] [string]$Exe,
  [int]$HoldSeconds = 20
)
```

Start `"$Exe" --supervisor --config $Config --state-dir $StateDir --run-id smoke`, wait for `worker-plan.json`, poll `"$Exe" --status --run-id smoke --state-dir $StateDir` every second until it prints a `State` line, then `"$Exe" --stop --run-id smoke --state-dir $StateDir`, wait up to `$HoldSeconds` for exit code 0. On timeout print the last 20 lines of `$StateDir/journal.jsonl` and exit 1.

- [ ] **Step 2: Verify the script against a missing config**

Run: `pwsh -File scripts/smoke-watchdog.ps1 -Config missing.json -StateDir $env:TEMP\smoke -Exe native/build/htmlppt-watchdog.exe`
Expected: non-zero exit with a message naming the missing config, no PowerShell stack trace.

- [ ] **Step 3: Document the run flow**

Add a "Running the watchdog" section to `native/README.md`: `--supervisor` needs `--config`; the worker is spawned automatically; `--status` shows state; `--stop` exits; the smoke script is the on-machine check; WPS must be installed and the deck closed.

- [ ] **Step 4: Commit**

```bash
git add scripts/smoke-watchdog.ps1 native/README.md
git commit -m "feat(scripts): add watchdog smoke script and document the run flow"
```

---

### Task 10: Manual acceptance record

**Files:**
- Modify: `native/README.md` (acceptance table)

- [ ] **Step 1: Run the smoke script on the real machine**

Run: `pwsh -File scripts/smoke-watchdog.ps1 -Config demo/watcher.json -StateDir demo/state -Exe native/build/htmlppt-watchdog.exe`
Expected: exit 0; `demo/state/journal.jsonl` shows `Supervisor start → Worker start → Monitoring → … → stop`.

- [ ] **Step 2: Record the result**

Add the date, WPS version, observed states and any deviation to the README acceptance table. A failed run is recorded with the failing state and the journal excerpt; the task stays open until the cause is known and filed.

- [ ] **Step 3: Commit**

```bash
git add native/README.md
git commit -m "docs: record watchdog smoke acceptance"
```

---

## Self-Review Notes

- Spec §4.3's five threads collapse into one bounded loop for 5C; `Win32CoverOverlay` keeps its own thread and IPC stays synchronous with a 0-ms receive. Splitting the remaining threads is 5D.
- Spec §7.5's `MsgWaitForMultipleObjectsEx` is not used yet: `WpsHost` pumps STA messages inside each COM call, and the loop polls. Recorded as a 5D item.
- Staging modes, compat-flag mirroring, WinEvent slide detection and supervisor-side Edge Job ownership stay out of scope (the rest of the upstream deferred list).
- Every Review Focus line has an owning test: #1 Task 2, #2 Task 4, #3 Task 6, #4 Task 6, #5 Task 8.
