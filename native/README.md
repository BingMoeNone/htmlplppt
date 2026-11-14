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
5-restarts-per-10-minutes budget, and a 200 ms/500 ms/1 s/2 s/5 s backoff with
jitter. `NextDeadline()` exposes the pending restart; `Tick()` dispatches it.
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
