# htmlppt-watchdog native

## Build on Linux/WSL

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

## Build on Windows with MSVC

```powershell
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64 -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --config Release --parallel
ctest --test-dir native/build -C Release --output-on-failure
```

## Layout

- `src/core/` is platform-neutral and must build on Linux.
- `src/core/deck_info.*` is the deck-info provider interface, JSON provider, and fingerprint.
- `src/core/resolve.*` resolves slide/title targets, HTML paths, and auto names.
- `src/core/state_machine.*` owns the platform-neutral lifecycle and failure policy.
- `src/core/failure_policy.*` owns failure escalation and the COM error window.
- `src/app/` contains the executable entry point.
- `src/app/cli.*` implements the diagnostic CLI.
- `tests/unit/` contains test cases registered with `HTMLPPT_TEST_CASE`.
- `third_party/` contains vendored single-header dependencies.

## CLI diagnostics

```bash
./native/build/htmlppt-watchdog --help
./native/build/htmlppt-watchdog --print-schema
./native/build/htmlppt-watchdog --validate --config watcher.json
./native/build/htmlppt-watchdog --resolve --config watcher.json --deck-info deck-info.json
./native/build/htmlppt-watchdog --print-config --config watcher.json
```

`--validate` checks config structure, deck existence, HTML paths, duplicate
slide mappings, duplicate stage names, and duplicate Edge profiles. A warning
only result exits 0; any error exits 1.

`--resolve` prints the final mapping table and exits 1 on resolution errors.
`--deck-info` supplies the slide count and titles on Linux/WSL and in CI, where
the WPS COM host is not available:

```json
{
  "slides": [
    { "index": 1, "title": "Cover", "name": "Slide1" },
    { "index": 2, "title": "Contract", "name": "Slide2" }
  ]
}
```

Slide indices must be contiguous starting at 1. Without `--deck-info`, numeric
slide targets resolve with a warning and title/name matches fail with an error.
The WPS COM implementation of `DeckInfoProvider` is a later plan.

`--map` appends a target. A duplicate slide mapping is rejected until
source-priority overrides land in a later plan. Relative HTML paths in
`--map` resolve against the config file directory, not the current working
directory.

Auto names are deterministic: `stageName` becomes
`target-<NN>-<ascii-slug>.html`; `edgeProfile` becomes
`htmlppt-<deck-fingerprint>-t<NN>`. Explicit values are preserved.

When the deck fingerprint is unavailable, auto Edge profiles use the
`nodeck` segment instead of a fingerprint.

## State machine contract (Plan 3)

`core/state_machine` is platform-neutral. It consumes events and emits
declarative actions; the Windows host executes them. It never calls COM,
Win32, or wall-clock APIs.

Events: slide changes, show start/end, cover hold/done, Edge ready/closed,
return request, timeouts, stop, cleanup done, power resume, config reload,
failures, supervisor loss.

Actions: cover fade/hide, Edge launch/kill, show exit/resume/restore, focus,
safe mode, legacy fallback, helper-thread stop, lock release, journal, and
diagnostic messages.

States: Boot, Warming, Monitoring, CoverIn, EdgeStarting, ShowExit, Live,
Returning, ReturnCoverOut, Stopping, Faulted, SafeMode, Exited.
`DegradedNoSupervisor` is reserved in the enum; supervisor loss is reported
as `StateSnapshot.degraded_no_supervisor` while the current lifecycle state
continues.

Failure policy:

- Transient failures retry with the configured backoff.
- COM failures that persist for `comErrorWindow` (2s) become structural.
- Switch failures hide the cover and kill the owned Edge job. The normal
  recovery path restores WPS and either self-heals (clear done) or keeps done
  per the target policy. Safe, legacy, or threshold-exceeded paths enter
  SafeMode instead and do not emit `RestoreShow`; legacy also emits a
  legacy-fallback action.
- Return failures hide the cover and enter Faulted; Edge is left for manual
  close, and `edgeClosed` only journals.
- Structural failures hide the cover, kill the owned Edge job, and enter
  SafeMode; `legacy` policy emits a legacy-fallback action.
- Stop moves to Stopping, emits cleanup actions, and enters Exited after
  `cleanupDone`. Supervisor loss continues by default or cleans up when
  configured.

Timers: cover hold safety, Edge ready 20s, Edge live 600s, return resume,
cover fade-out completion, and retry backoff. `NextDeadline()` exposes the
next timer; `Tick()` dispatches the due timeout.

Host contracts (Plan 4/5):

- `showStarted` means a genuinely new slideshow. The `SlideShowBegin` produced
  by `ResumeShow` must not be forwarded as `showStarted`, or the state machine
  clears `done` and a target can re-trigger.
- Events carry no trigger generation. The host must tag or filter events so a
  late `edgeReady`, `edgeClosed`, or `showEnded` from a previous trigger cannot
  reach the current generation.
- `ShowExit` has no built-in timer. If `ExitShow` does not produce `showEnded`,
  the host must synthesize a switch failure; otherwise the cover can remain
  visible.
- Execute action batches in order and abort the remainder when an action fails
  (a failed `ResumeShow` must not be followed by `CoverFadeOut`).
- `KillEdge` and `RestoreShow` must be idempotent.
- COM failures are evaluated when the next COM failure event arrives; the
  error window is not a timer.
- Cover fade/hold and focus timings are host-facing fields; the host maps the
  Plan 2 configuration into `StateMachineConfig` and `TargetPlan`.

## Supervisor core (Plan 4A)

`core/supervisor_protocol` defines the worker-to-supervisor wire format: frame
magic/version/sequence/timestamp/type/length plus CRC32 over the header and
payload, with a 64 KiB payload limit. `SequenceTracker` reports heartbeat
stalls.

`core/blackbox` is a fixed 96-byte-record ring buffer; Plan 4B maps it onto
shared memory. `core/worker_health` tracks registered thread roles and reports
stale heartbeats.

`core/supervisor` is a declarative decision engine. It emits `StartWorker`,
`RequestDump`, `TerminateWorker`, `RestartWorker`, `EnterSafeMode`,
`RunLegacyFallback`, and `StopComplete`. It uses a 2 s heartbeat timeout, a
12 s loop-progress stall timeout (above the longest permitted blocking COM
action), a 5-restarts-per-10-minutes budget, and a 200 ms/500 ms/1 s/2 s/5 s
backoff with jitter. `NextDeadline()` exposes the pending restart; `Tick()`
dispatches it.
The host must call `Tick()` at least every 100 ms (design spec 6.1);
`NextDeadline()` exposes restart deadlines but not the heartbeat-stall check.
`Start()` and a late `Started` are ignored while a restart is scheduled; the
scheduled restart is authoritative.

`core/single_instance` derives a stable instance fingerprint and mutex name
from the canonical config directory plus config hash, formats the
`watcher.lock` JSON line, and recognizes legacy lock lines.
`core/journal_reconcile` decides whether startup should clean up, take over,
restore WPS, or enter SafeMode from the last journal state and observed
resources.

For reconciliation, outcomes that request actions populate `kill_edge`,
`hide_overlay`, and `resume_wps` from the observed facts; the host must apply
every true flag. `Clean` preserves the last slide for diagnostics but does not
set `resume_wps`; the empty-journal path has no facts to apply.

Plan 4B maps these cores onto Windows Job Objects, named pipes, shared memory,
MiniDump, process launch/termination, and CLI mode dispatch. No Windows API is
used in Plan 4A.

## Windows platform primitives (Plan 4B-1)

`platform/win32/process_job` creates a Job Object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, launches the worker suspended, assigns it
to the job, resumes it, terminates the job, and reports process liveness.

`platform/win32/named_pipe` implements an overlapped duplex pipe transport for
Plan 4A frames. `Receive` reads a 30-byte header, validates the payload length,
reads the payload, and decodes it with `DecodeFrame`.

`platform/win32/shared_blackbox` maps an 8 MB-capable file mapping with a
fixed header and `BlackboxRecord` array. `Append` is single-writer and
lock-free; `Snapshot` reads in chronological order and reports dropped
records.

`platform/win32/crash_dump` writes a MiniDump for a live process through
`MiniDumpWriteDump`.

Windows gate (MSVC 19.51 / Visual Studio 18 Build Tools):

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

Expected for the Plan 4B-1 primitives in isolation: `htmlppt_core_tests`
189/189 (two pre-existing journal cases are excluded on Windows). The current
Plan 4B-2 gate runs `htmlppt_win32_tests` 22/22, Windows CTest 9/9, and
MSVC `/WX` clean.

## Supervisor and worker host (Plan 4B-2)

The same EXE runs as supervisor and worker.

- `--supervisor --state-dir <dir> --run-id <id>` creates the stop event, single-instance mutex, shared black box, named-pipe server, and worker Job. It resolves the config with the CLI overrides applied, writes `worker-plan.json` and the effective `worker-config.json` into the state dir, then launches the worker with that config; it expects heartbeats with a 2 s timeout, restarts the worker with the Plan 4A budget/backoff, and exits after `--stop`. A worker restart recreates the pipe server before relaunching the worker.
- `--worker --run-id <id>` reads the plan and the effective config, opens the deck in WPS, starts the slideshow, and runs the state machine loop, heartbeating the current state while the loop works. It exits on a Stop frame or after a clean state-machine exit; a supervisor loss is reported to the machine and the loop continues.
- `--stop --run-id <id>` sets the named stop event and waits for the supervisor to finish worker termination, journal flush, and handle cleanup (10 s cap) before returning.
- `--status --run-id <id> --state-dir <dir>` prints the run id, state, worker PID, last event and the last black-box records (each line ends with the state name, e.g. `[Monitoring]`) plus the journal path.
- `--diagnostics --run-id <id> --state-dir <dir> --out <path>` writes a self-contained package: `manifest.txt`, `journal.jsonl`, `watch-log.txt`, the plan/effective config, a black-box snapshot (`blackbox.txt`, when the run still owns it) and any crash dumps.
- `--dump-helper <pid> <path>` writes a MiniDump.

The shared black box is written only by the worker; the supervisor reads a read-only view. On Linux these host modes print a Windows-only error and exit 2; the diagnostic CLI is unchanged.

Every supervisor case in `htmlppt_win32_tests` launches a worker that opens the
deck in WPS and starts a real slideshow when WPS is present. The end-to-end case
(`win32_supervisor_starts_and_stops`) additionally asserts the show-start
record, so when the `Kwpp.Application` COM ProgID is not registered it prints
`SKIP: WPS not installed` and passes; the other cases still exercise the
supervisor and worker lifecycle around that attempt. The cases terminate only
the WPS processes they started, so repeated runs do not accumulate automation
instances.

Windows gate: `htmlppt_core_tests` 284/284, `htmlppt_win32_tests` 61/61 (the
WPS case is skipped without WPS), CTest 9/9, `-Werror` clean.

## Running the watchdog

The supervisor and the worker are the same EXE. A run needs a config, a state
directory, and a run id:

```powershell
native\build\htmlppt-watchdog.exe --supervisor --config watcher.json --state-dir .\state --run-id demo
```

`--supervisor` requires `--config`. It resolves the config once, with the same
`--profile`, `--map`, and `--scheduling` overrides `--validate` uses, and
writes `worker-plan.json` and the effective `worker-config.json` into the state
directory before anything is launched. The worker is spawned automatically; do
not run `--worker` by hand. The worker opens the deck in WPS, starts the
slideshow, and drives the cover, Edge, and show actions while heartbeating its
state into the shared black box.

A slide change is only observable through the WPS COM poll, so the poll interval
is the trigger latency: the loop polls every 20 ms while it is `Monitoring` a
running show and falls back to 100 ms otherwise (`WorkerHostConfig::active_poll_interval`).

### WPS COM events (probe result)

`tools/probe-wps-events.cpp` (built as `htmlppt-probe-wps-events` on Windows)
answers whether the watchdog could take slide changes from a COM event instead
of polling:

```powershell
native\build\htmlppt-probe-wps-events.exe demo\deck.pptx
```

Finding on WPS 12.1: the automation server exposes exactly one connection point,
`{914934C2-5A91-11CF-8700-00AA0060263B}`, and it is connectable. The registered
type library does not resolve `SlideShowNextSlide`, but a sink that accepts any
DISPID sees exactly two events per slide change (DISPID 2013 with one argument,
DISPID 2019 with two) and none while the show is idle.

Decision: the event sink is **not** integrated. The loop already polls every
20 ms while `Monitoring`, so an event would save at most one poll interval --
the remaining latency is the COM round trip itself -- while integration costs a
connection point on the worker's STA, a message pump in the loop's wait, and
advise/unadvise lifetime handling. The probe stays in the tree so the decision
can be revisited if the cadence is ever raised or WPS exposes a named event
interface.

### Fault injection

`HTMLPPT_FAULTS` in the supervisor's environment enables the spec 6.6 switches,
so the resilience paths can be exercised on a real machine without wedging WPS:

| switch | effect |
|---|---|
| `com-hang` | every COM query fails, driving the 2 s error window towards SafeMode |
| `com-462` | the same path with the classic WPS busy HRESULT |
| `edge-slow` | Edge readiness is held back 3 s (cold start); the machine's EdgeStarting deadline still bounds it |
| `cover-hang` | the cover never reports its hold, so the machine's cover deadline owns recovery |
| `focus-deny` | activation always fails, so Edge never becomes ready |
| `supervisor-loss` | the worker is told the supervisor is gone at start; the loop must continue |
| `sleep-resume` | injects a 60 s suspend gap and a resume, exercising the deadline shift |
| `multi-monitor` | injects a display-topology change (half-size reference metrics) |

The worker journals `faults: <list>` into the black box and prints a warning, so
an injected run is never silent:

```powershell
$env:HTMLPPT_FAULTS = 'edge-slow'
native\build\htmlppt-watchdog.exe --supervisor --config demo\watcher.json --state-dir demo\state --run-id demo
```

`HTMLPPT_BENCHMARK=1` (worker environment) makes the worker write
`benchmark.txt` next to `worker-config.json` with per-step and per-COM-query
p50/p99/max latencies, so a real run's numbers are evidence rather than a guess:

```text
steps 118 p50 31ms p99 140ms max 233ms
com   118 p50  3ms p99   9ms max  19ms
```

The cover window also re-covers on `WM_DISPLAYCHANGE`, `WM_DPICHANGED` and
`WM_POWERBROADCAST` resume: the loop's wait pumps messages, so a topology change
or resume reaches the overlay immediately instead of waiting for the next COM
call. A resume also re-bases every pending deadline by the measured suspend gap
(`power resume: shifted deadlines by <n> ms`), so waking up does not look like a
pile of expired timers; a display change updates the reference metrics used for
show-window scoring.

### Config editor (`--config-ui`)

```powershell
native\build\htmlppt-watchdog.exe --config-ui --config watcher.json
```

Windows only. Add `--deck-info deck-info.json` on a machine without WPS, exactly
like `--validate`. The window is a thin Win32 view over `core/config_editor`,
which edits the effective profile, validates through the same resolver a run
uses, and saves atomically. `--allow-outside-folder` is forwarded to a run
launched from the window, so the editor and the run resolve HTML paths with the
same permission.

1. The table lists one row per effective target: slide, label, HTML, assets
   mode, and the resolver's state. An error row carries its first issue in the
   STATE column.
2. Selecting a row fills the right-hand pane: slide, label, HTML, an assets
   mode (`inherit` clears the target's own block so the stage mode applies), and
   the enabled switch. Edit the fields and press **Apply**; the status line
   reports `applied`, or the first blocking reason when the refresh leaves Save
   closed.
3. **Add** appends a target with a placeholder HTML, **Remove** asks for
   confirmation and drops the selected target, and **Validate** re-runs the
   resolver and shows the first issue -- including a config-wide error such as a
   missing deck, which has no row of its own.
4. **Save** stays disabled while the model is clean or blocked by an error. It
   writes a sibling temporary file and only then replaces the config, copying
   the previous contents to `<name>.bak`; a failed write never truncates the
   config. The gate closes again once the save succeeds.
5. Closing with unsaved edits asks Yes/No/Cancel. Yes saves first -- only while
   the Save gate is open, so a blocked config is never persisted -- and closes
   only when the save succeeds; a failed save stays open and shows the reason.
   No discards the edits and closes, and Cancel keeps the window and its edits.
   The window declares a minimum size and follows `WM_DPICHANGED`, so the pane
   and its Apply button never cover the status line.
6. **Start** launches a supervisor for this config as a detached process (the
   same EXE re-entered with `--supervisor`), and **Stop** asks it to shut down
   through `--stop` and reports the exit code. Start stays disabled unless the
   config is clean and no supervisor owns the run's state directory; it also
   saves unsaved edits first, through the same gate the Save button uses, so a
   run always reads the file the editor shows. The status line shows the run id
   (`ui-<timestamp>`) and then the state the run last journalled, spelled the
   same way `--status` spells it. A 500 ms poll reaps the child, so a supervisor
   that exits on its own reopens Start and reports `watchdog stopped`.

An editor-launched run writes its state to `<config-dir>\.htmlppt-run`, a hidden
sibling of the config file, so the plan and journal are discoverable next to the
config instead of mixing into the user's folder. That is deliberately different
from the CLI's per-user default (`%LOCALAPPDATA%\htmlppt-watchdog`) and from the
smoke script's explicit `-StateDir`, because the editor must not share one state
directory across unrelated configs. Closing the editor leaves a running
supervisor alive (a UI crash must not kill the watchdog); stop it with
`--stop --run-id <id> --state-dir <config-dir>\.htmlppt-run`.

**Profile-based configs are read-only for saving in v1.** When the file uses
`profiles` or `activeProfile`, the editor loads the effective profile but Save
stays disabled and the status line says so: the model serializes the effective
profile only, so saving would flatten the file and drop the other profiles. Edit
the profile blocks in the JSON itself.

A missing or broken config is not fatal: the window opens with the loader's
first issue in the status line instead of exiting. The deck is config-wide and
is not editable in the window, so Save stays blocked until the file is repaired
outside the window. The auto-mapping wizard from spec 9.5 is still not built; the
editor sits on `ConfigEditorModel` so it can be added later.

### Threading model

The worker is a single STA thread that owns the COM objects, the cover window,
and the state loop. A helper thread only sends heartbeat frames (under a send
mutex) so a blocking COM action cannot starve the supervisor.

The loop's wait is `PumpingClock` (`platform/win32/pumping_clock`): it waits with
`MsgWaitForMultipleObjectsEx` and dispatches queued messages in slices of at most
50 ms. That matters because the loop thread is the apartment that hosts the COM
objects and the cover window -- a plain `sleep_until` would leave their callbacks
queued until the next COM call. It is deliberately not a multi-thread rework: the
heartbeat helper already covers the supervisor-side liveness requirement, and a
split COM/UI/IPC model only pays off if the slide source becomes an event sink.

From a second shell, inspect the run and stop it:

```powershell
native\build\htmlppt-watchdog.exe --status --run-id demo --state-dir .\state
native\build\htmlppt-watchdog.exe --stop --run-id demo --state-dir .\state
```

`--status` prints the last black-box records (state number and name) and the
journal path. `--stop` sets the stop event and returns after the supervisor has
terminated the worker, flushed the journal, and closed its handles; it exits 0
after a clean stop. A config or deck that cannot be resolved fails before a worker is
launched, journals the reason, and exits 1.

WPS Presentation must be installed and the deck must be closed before a run:
the supervisor reads the deck facts through COM and the worker opens that deck
and starts the show. The supervisor's planning step needs a responsive WPS (or
`--deck-info`), and a wedged WPS is a prerequisite failure, not a watchdog
failure. Hang detection fires after ~12 s of frozen loop progress because 5C
permits blocking COM actions; a wedged WPS is restarted within that budget.

The on-machine check is `scripts/smoke-watchdog.ps1`. It starts a run with the
run id `smoke`, waits for `worker-plan.json`, polls `--status` once a second
until the worker reaches `Monitoring` (state 2) -- i.e. the deck is open and the
show is actually running -- then runs `--stop` and requires the supervisor to
exit 0. An earlier faulted/safe-mode state fails immediately instead of waiting
out the window. `-ExpectState` raises or lowers the required state:

```powershell
pwsh -File scripts/smoke-watchdog.ps1 -Config watcher.json -StateDir .\state -Exe native\build\htmlppt-watchdog.exe
```

The script targets Windows PowerShell 5.1 and later, so a host without `pwsh`
can run the same command through `powershell -File` instead.

The script tears the supervisor down and prints the last 20 journal records
when any step fails. Each run step is bounded by `-HoldSeconds` (default 60);
teardown after a failure adds a fixed ~25 s before the journal tail. A pass
means the supervisor resolved the config, launched the worker, the worker
opened the deck and started the show, and the run stopped cleanly -- not merely
that a heartbeat arrived. Triggering the HTML still needs the presenter (or a
slide-driving COM script); the check opens a real slideshow, so run it only on
a machine where that is acceptable; it is not part of `ctest`.

`--stop` exits the slideshow during cleanup (`ExitShow`) but never quits WPS:
`PresentationHost::Release` promises to leave the application alone, so the
editor stays open with the deck loaded.

Edge's `--user-data-dir` is a full browser profile (~15 MB). The worker creates
it under `%TEMP%\htmlppt-<run-id>` and removes the directory on exit, so the
state directory holds only `worker-plan.json`, `worker-config.json`, and
`journal.jsonl`.

### Legacy compatibility mirror

While a run owns the config directory, the worker keeps the VBS-era contract
alive: it writes `watcher.lock` next to the config file and refreshes it every
5 s (the legacy watcher treats a lock older than 15 s as stale), appends the
runtime journal to `watch-log.txt` when it exits, and removes the flag files it
owns. `stop-live.vbs`-style tooling and the troubleshooting steps in
`docs/04-reproduce.md` therefore keep working against a native run. A worker
that is killed leaves the lock behind, which is exactly the stale-lock case the
legacy watcher already handles.

### Asset staging

`targets[].assets.mode` decides what Edge opens:

- `file` (default): the HTML is copied to
  `%TEMP%\htmlppt-<run-id>\stage\<stageName>` and Edge opens that copy.
- `tree`: the same, plus the whole `assets.root` tree (default: the HTML's
  folder), preserving relative paths, filtered by `assets.include` /
  `assets.exclude` globs (a non-empty include whitelists; exclude always wins;
  `*` and `**` match across `/`), and capped by `assets.maxBytes` (0 = no cap).
- `direct`: Edge opens the resolved source path in place, with no copy.

Staging runs when the cover is fading in. A missing source or an over-cap tree
fails the trigger -- it is journalled and the retry policy owns the next attempt
-- instead of opening a broken page. Destination names come from `stageName`, so
a CJK source filename still produces a predictable URL, and the stage directory
is removed with the run's temp directory.

`stage.mode` / `stage.root` are the defaults for targets without their own
`assets` block, and `--stage-mode` overrides that default for the run; a target's
own `assets` block always wins.

### Display, focus, and prewarm

- `display.monitor` (`"primary"`, `"secondary"`, or a 1-based index) selects the
  reference monitor for show-window scoring and focus. The cover itself always
  spans the virtual screen, so every display goes dark; an out-of-range selector
  falls back to the primary with a `display:` record in the black box.
- `focus.strategy` (`auto`, `attach-alt`, `switch`) selects the activation chain:
  `auto` runs the full multi-stage chain (topmost, restore, foreground, switch,
  thread attach, Alt unlock), `attach-alt` and `switch` restrict it to that one
  technique. Unknown values are a config error.
- `edge.prewarmProfile: true` launches each target's Edge profile headless for up
  to 2 s before the show starts, so the browser's first real launch does not pay
  the profile-initialization cost. The prewarm processes are terminated and never
  show a window.

### Live page contract

`templates/live-page.html` is the reference page. The watchdog watches the Edge
window title for `TEF-LIVE-RETURN`; on that signal it fades the cover in, closes
Edge under the cover (`KillEdge`), resumes the WPS show, and fades the cover
out. Because the watchdog now closes Edge itself, a page that only renames its
title cannot strand a full-screen browser on top of the resumed deck. Calling
`window.close()` after a short delay is still recommended and is what the
template does. See `references/live-content-trigger.md` for the full protocol.

Pass ASCII paths for `--config`, `--state-dir`, and `-StateDir`: the native CLI
converts path arguments through the ANSI code page, so a non-ASCII path aborts
the process before the run starts. Deck paths inside the config file may still
be UTF-8.

## Cover, focus, Edge, and compatibility core (Plan 5B-1)

`core/cover_timeline` owns fade-in, minimum hold, maximum hold safety, and
fade-out. `core/cover_controller` drives a `CoverOverlay` implementation and
re-asserts topmost when the hold edge is reached.

`core/window_system` is the platform-neutral window descriptor/control
interface. `core/focus_model` ranks WPS slideshow windows by class, title,
popup style, and fullscreen traits, selects the largest visible Edge window
for a PID, and recognizes `TEF-LIVE-RETURN`. `core/focus_controller` polls
`Activate` until the window is foreground or the timeout expires. The polling
interval is clamped to at least 1 ms, sleeps no later than the deadline, and
WPS score ties are broken by larger area, then smaller window id.

`core/edge_spec` builds a `file://` URL, percent-encodes UTF-8 paths, applies
the default or configured Chromium flags, and assigns a per-target profile
directory. `core/edge_controller` owns launch/active/kill over an
`EdgeLauncher` interface. `ResolvedTarget::html_path` and all Edge arguments
are UTF-8 strings; use `PathFromUtf8` before filesystem work and `Utf8Path`
when producing strings. Plan 5B-2 must convert argv with UTF-8-to-UTF-16, not
the Windows ANSI code page.

`core/compat_flags` writes owner-tracked legacy flags (`watcher.lock`,
`cover-*`, `agent-*`, `return-request`, `edge-closed`) without altering the
legacy payload content; ownership is stored in a sibling `.owner` file.
`DeleteOwned` keeps the owner when payload removal fails, `WriteFlag` writes
the owner first and rolls it back if the payload fails, and `DeleteAllOwned`
returns the number of owned flags that could not be deleted.
`LegacyLog` in `core/compat_flags.h` appends timestamped `watch-log.txt`,
`bring-log.txt`, and `cover-log.txt` lines.

These units are platform-neutral and tested on Linux. Plan 5B-2 maps them onto
Win32 layered windows, `EnumWindows`, foreground unlocking, and
`CreateProcessW`/Job Objects.

Plan 5B-1 Linux gate (historical): 244/244 unit cases, CTest 8/8,
`npm test` 3/3. The current Plan 5B-2 Linux gate is 247/247.
The Windows gate (MSVC `/WX`, real `EnumWindows`, layered window, Edge) is
part of Plan 5B-2.

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

## WPS COM host (Plan 5A)

`platform/win32/com_dispatch` provides the STA COM apartment, RAII
`IDispatch` pointers, variant conversion, named late-bound calls, and HRESULT
classification. `HostStatus::Busy` covers transient failures including WPS
error 462; `HostStatus::Unavailable` covers missing or disconnected COM
servers; `HostStatus::Failed` is structural.

`platform/win32/wps_host` implements the platform-neutral
`core/presentation_host.h` contract and the existing `DeckInfoProvider`
interface through late-bound `Kwpp.Application` calls:

- `Launch` creates WPS, makes it visible, and opens the deck read-only with
  three attempts.
- `StartShow` sets `StartingSlide=1`, `ShowWithAnimation=True`, calls
  `SlideShowSettings.Run`, and waits up to 2.5 s for a slideshow window.
- `QueryShow` reads `SlideShowWindows.Count` and
  `SlideShowWindow.View.CurrentShowPosition`.
- `ExitShow`, `ResumeShow`, `JumpTo`, and `FocusShow` use the same retry
  timings as the legacy watcher (300 ms between attempts).
- `Load` reads `Slides.Count`, `Slide.Name`, and
  `Shapes.Title.TextFrame.TextRange.Text` and closes a deck opened only for
  diagnostics.

On Windows, `--validate` and `--resolve` use WPS automatically when
`--deck-info` is omitted. `--deck-info` remains the deterministic path for
CI, Linux/WSL, and machines without WPS; if WPS is unavailable the CLI emits
a warning and falls back to the previous limited-resolution behavior.

Manual acceptance on a Windows machine with WPS 12.1:

1. Run `htmlppt-watchdog.exe --resolve --config watcher.json` with a real
   deck and both a numeric and a title-based target. Expected: the table
   resolves every target and prints no deck-info warnings. WPS may open
   briefly; it must not start a slideshow.
2. Run `--validate` with an out-of-range numeric target. Expected: a slide
   range error even without `--deck-info`.
3. Re-run `--resolve --deck-info deck-info.json`. Expected: identical output
   and no WPS process is created.
4. Run `--resolve` with WPS closed and then with WPS unavailable. Expected: a
   warning that names `--deck-info`, not a crash or hang.

`htmlppt_win32_tests` covers the COM helpers and the WPS host with an
in-process fake `IDispatch` tree, so the automated gate does not require WPS.

Windows gate: `htmlppt_core_tests` 189/189, `htmlppt_win32_tests` 28/28,
CTest 9/9, `/WX` clean.
