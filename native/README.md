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
Returning, ReturnCoverOut, Stopping, Faulted, SafeMode, Exited. Supervisor
loss is reported as `StateSnapshot.degraded_no_supervisor` while the current
lifecycle state continues.

Failure policy:

- Transient failures retry with the configured backoff.
- COM failures that persist for `comErrorWindow` (2s) become structural.
- Switch failures hide the cover, kill the owned Edge job, restore WPS, and
  either self-heal (clear done) or keep done per the target policy.
- Return failures hide the cover and enter Faulted; Edge is left for manual
  close, and `edgeClosed` only journals.
- Structural failures clean up and enter SafeMode; `legacy` policy emits a
  legacy-fallback action.
- Stop moves to Stopping, emits cleanup actions, and enters Exited after
  `cleanupDone`. Supervisor loss continues by default or cleans up when
  configured.

Timers: cover hold safety, Edge ready 20s, Edge live 600s, return resume,
cover fade-out completion, and retry backoff. `NextDeadline()` exposes the
next timer; `Tick()` dispatches the due timeout.

Host contract: `showStarted` means a genuinely new slideshow. The
`SlideShowBegin` produced by `ResumeShow` must not be forwarded as
`showStarted`, or the state machine clears `done` and a target can re-trigger.
`LaunchEdge` retries are preceded by `KillEdge`, so the host's `KillEdge`
must be idempotent.
