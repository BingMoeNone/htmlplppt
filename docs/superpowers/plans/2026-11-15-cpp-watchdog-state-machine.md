# C++ Watchdog State Machine and Failure Policy Implementation Plan (Plan 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the platform-neutral watchdog state machine and failure policy: lifecycle transitions, trigger/return guards, timeout scheduling, transient/switch/return/structural failure handling, safe mode, faulted recovery, cleanup, and supervisor-loss behavior.

**Architecture:** `core/state_machine_types` defines the event, action, run-plan, and snapshot vocabulary. `core/failure_policy` owns failure escalation and the COM error window. `core/state_machine` is the single state owner: it consumes events, returns declarative actions for the Windows host to execute, exposes the next timer deadline, and never calls COM, Win32, or wall-clock APIs. Plans 4 and 5 wire the supervisor, WPS host, overlay, focus, and Edge job to these actions.

**Tech Stack:** C++20, CMake/CTest, the Plan 1 `Clock`/`RetryPolicy` primitives, the Plan 2 config/resolve models, and the existing zero-dependency test harness.

## Global Constraints

- Target Windows 10/11 x64; Plan 3 must build and test on Linux/WSL.
- C++20; MSVC `/W4 /permissive- /utf-8`; GCC/Clang `-Wall -Wextra -Wpedantic`. The gate uses `-Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code lives under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only. Configuration content is UTF-8 at runtime.
- `core/` must not include Windows-specific headers. No COM or Win32 calls in this plan.
- Monotonic time only. The state machine uses `ClockPoint`/`Duration` and the injected `Clock`; no wall-clock time.
- The state machine is the single state owner. Hosts execute actions and report facts as events; they do not mutate state directly.
- Default timeouts (from the design spec): startup ignore 1s, return ignore 2s, cover fade 300ms, cover hold 1000ms, cover max hold 10000ms, Edge ready 20s, Edge max live 600s, focus 6s, COM error window 2s, consecutive trigger failure threshold 3.
- The existing suite (97 unit cases, CTest 8/8) must stay green.

## File Structure

Create:

- `native/src/core/state_machine_types.h` - Action/ActionKind, Event/EventKind, TimeoutKind, FailureSource, TargetPlan, RunPlan, StateMachineConfig, StateSnapshot, event factories.
- `native/src/core/state_machine_types.cpp` - event factory definitions.
- `native/src/core/failure_policy.h` - `EscalateFailure`, `ComErrorWindow`.
- `native/src/core/failure_policy.cpp` - failure mapping and the contiguous-COM-error window.
- `native/src/core/state_machine.h` - `StateMachine` public API and private transition helpers.
- `native/src/core/state_machine.cpp` - transition engine, guards, timers, retries, failure outcomes.
- `native/tests/unit/failure_policy_test.cpp` - failure-policy unit tests.
- `native/tests/unit/state_machine_test.cpp` - state-machine unit tests.

Modify:

- `native/README.md` - document the state-machine contract and action/event vocabulary.
- `docs/superpowers/plans/2026-11-15-cpp-watchdog-state-machine.md` - append as-built amendments after execution.

---

### Task 1: State-Machine Types and Failure Policy Primitives

**Files:**
- Create: `native/src/core/state_machine_types.h`
- Create: `native/src/core/state_machine_types.cpp`
- Create: `native/src/core/failure_policy.h`
- Create: `native/src/core/failure_policy.cpp`
- Create: `native/tests/unit/failure_policy_test.cpp`

**Interfaces:**
- Consumes: `ClockPoint`, `Duration`, `Clock` from `core/clock.h`; `RetryConfig` from `core/retry_policy.h`; `State`, `TriggerMode`, `OnFailurePolicy`, `FailureKind` from `core/types.h`.
- Produces:
  - `enum class ActionKind { CoverFadeIn, CoverFadeOut, CoverHide, LaunchEdge, KillEdge, ExitShow, ResumeShow, RestoreShow, FocusEdge, FocusWps, EnterSafeMode, LegacyFallback, StopHelperThreads, ReleaseLock, Journal, Diagnostic }`
  - `struct Action { ActionKind kind; int slide; std::string detail; }`
  - `enum class EventKind { SlideChanged, ShowStarted, ShowEnded, CoverHold, CoverDone, EdgeReady, EdgeClosed, ReturnRequested, Timeout, StopRequested, CleanupDone, PowerResume, ConfigReloaded, Failure, SupervisorLost }`
  - `enum class TimeoutKind { CoverHold, EdgeReady, EdgeLive, ReturnResume, CoverDone, Retry }`
  - `enum class FailureSource { Com, Focus, Edge, Title, Other }`
  - `enum class SupervisorLossPolicy { Continue, Cleanup }`
  - `struct Event`, `struct TargetPlan`, `struct RunPlan`, `struct StateMachineConfig`, `struct StateSnapshot`
  - Event factories: `MakeSlideChanged`, `MakeShowStarted`, `MakeShowEnded`, `MakeCoverHold`, `MakeCoverDone`, `MakeEdgeReady`, `MakeEdgeClosed`, `MakeReturnRequested`, `MakeTimeout`, `MakeStopRequested`, `MakeCleanupDone`, `MakePowerResume`, `MakeFailure`, `MakeSupervisorLost`
  - `FailureKind EscalateFailure(State state)`
  - `class ComErrorWindow { explicit ComErrorWindow(Duration window); void record(ClockPoint); void reset(); bool expired(ClockPoint) const; bool active() const; }`

- [ ] **Step 1: Write the failing failure-policy tests**

Create `native/tests/unit/failure_policy_test.cpp`:

```cpp
#include "test_main.h"

#include "core/failure_policy.h"

using htmlppt::ComErrorWindow;
using htmlppt::Duration;
using htmlppt::FailureKind;
using htmlppt::FakeClock;
using htmlppt::State;

HTMLPPT_TEST_CASE(escalate_failure_maps_switch_states) {
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::CoverIn) == FailureKind::Switch);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::EdgeStarting) ==
                FailureKind::Switch);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::ShowExit) ==
                FailureKind::Switch);
}

HTMLPPT_TEST_CASE(escalate_failure_maps_return_states) {
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Live) == FailureKind::Return);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Returning) ==
                FailureKind::Return);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::ReturnCoverOut) ==
                FailureKind::Return);
}

HTMLPPT_TEST_CASE(escalate_failure_maps_structural_states) {
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Boot) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Warming) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Monitoring) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Faulted) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::SafeMode) ==
                FailureKind::Structural);
  HTMLPPT_CHECK(htmlppt::EscalateFailure(State::Exited) ==
                FailureKind::Structural);
}

HTMLPPT_TEST_CASE(com_error_window_expires_at_boundary) {
  FakeClock clock;
  ComErrorWindow window(Duration{2000});
  HTMLPPT_CHECK(!window.active());
  window.record(clock.now());
  HTMLPPT_CHECK(window.active());
  clock.advance(Duration{1999});
  HTMLPPT_CHECK(!window.expired(clock.now()));
  clock.advance(Duration{1});
  HTMLPPT_CHECK(window.expired(clock.now()));
}

HTMLPPT_TEST_CASE(com_error_window_reset_clears_state) {
  FakeClock clock;
  ComErrorWindow window(Duration{2000});
  window.record(clock.now());
  window.reset();
  HTMLPPT_CHECK(!window.active());
  HTMLPPT_CHECK(!window.expired(clock.now()));
}

HTMLPPT_TEST_CASE(com_error_window_keeps_first_error_time) {
  FakeClock clock;
  ComErrorWindow window(Duration{2000});
  window.record(clock.now());
  clock.advance(Duration{1500});
  window.record(clock.now());
  clock.advance(Duration{499});
  HTMLPPT_CHECK(!window.expired(clock.now()));
  clock.advance(Duration{1});
  HTMLPPT_CHECK(window.expired(clock.now()));
}
```

- [ ] **Step 2: Run the tests and verify they fail**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: compilation fails because `core/failure_policy.h` does not exist.

- [ ] **Step 3: Add the type vocabulary**

Create `native/src/core/state_machine_types.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/retry_policy.h"
#include "core/types.h"

namespace htmlppt {

enum class ActionKind {
  CoverFadeIn,
  CoverFadeOut,
  CoverHide,
  LaunchEdge,
  KillEdge,
  ExitShow,
  ResumeShow,
  RestoreShow,
  FocusEdge,
  FocusWps,
  EnterSafeMode,
  LegacyFallback,
  StopHelperThreads,
  ReleaseLock,
  Journal,
  Diagnostic,
};

struct Action {
  ActionKind kind = ActionKind::Journal;
  int slide = 0;
  std::string detail;
};

enum class EventKind {
  SlideChanged,
  ShowStarted,
  ShowEnded,
  CoverHold,
  CoverDone,
  EdgeReady,
  EdgeClosed,
  ReturnRequested,
  Timeout,
  StopRequested,
  CleanupDone,
  PowerResume,
  ConfigReloaded,
  Failure,
  SupervisorLost,
};

enum class TimeoutKind {
  CoverHold,
  EdgeReady,
  EdgeLive,
  ReturnResume,
  CoverDone,
  Retry,
};

enum class FailureSource {
  Com,
  Focus,
  Edge,
  Title,
  Other,
};

enum class SupervisorLossPolicy {
  Continue,
  Cleanup,
};

struct Event {
  EventKind kind = EventKind::Timeout;
  ClockPoint at{};
  int old_slide = 0;
  int new_slide = 0;
  TimeoutKind timeout = TimeoutKind::Retry;
  FailureKind failure = FailureKind::Transient;
  FailureSource failure_source = FailureSource::Other;
  std::string detail;
};

struct TargetPlan {
  int slide = 0;
  std::string label;
  std::string edge_profile;
  TriggerMode trigger = TriggerMode::LeaveForward;
  OnFailurePolicy on_failure = OnFailurePolicy::SelfHeal;
  std::optional<int> max_consecutive_trigger_failures;
  std::optional<Duration> cover_max_hold;
  bool enabled = true;
};

struct RunPlan {
  std::string run_id;
  int slide_count = 0;
  std::vector<TargetPlan> targets;  // enabled targets, sorted by slide
};

struct StateMachineConfig {
  Duration startup_ignore{1000};
  Duration return_ignore{2000};
  // Host-facing overlay/focus timings; the state machine does not read
  // cover_fade, cover_hold, or focus_timeout directly.
  Duration cover_fade{300};
  Duration cover_hold{1000};
  Duration cover_max_hold{10000};
  Duration edge_ready_timeout{20000};
  Duration edge_max_live{600000};
  Duration focus_timeout{6000};
  Duration com_error_window{2000};
  int max_consecutive_trigger_failures = 3;
  OnFailurePolicy default_on_failure = OnFailurePolicy::SelfHeal;
  SupervisorLossPolicy supervisor_loss = SupervisorLossPolicy::Continue;
  RetryConfig retry;
};

struct StateSnapshot {
  State state = State::Boot;
  int prev_slide = -1;
  std::optional<int> current_target_slide;
  int consecutive_trigger_failures = 0;
  std::uint64_t trigger_generation = 0;
  bool return_prepared = false;
  bool degraded_no_supervisor = false;
  bool safe_mode = false;
  bool show_active = false;
  std::optional<ClockPoint> ignore_until;
};

Event MakeSlideChanged(ClockPoint at, int old_slide, int new_slide);
Event MakeShowStarted(ClockPoint at);
Event MakeShowEnded(ClockPoint at);
Event MakeCoverHold(ClockPoint at);
Event MakeCoverDone(ClockPoint at);
Event MakeEdgeReady(ClockPoint at);
Event MakeEdgeClosed(ClockPoint at);
Event MakeReturnRequested(ClockPoint at);
Event MakeTimeout(ClockPoint at, TimeoutKind kind);
Event MakeStopRequested(ClockPoint at, std::string source);
Event MakeCleanupDone(ClockPoint at);
Event MakePowerResume(ClockPoint at);
Event MakeFailure(ClockPoint at, FailureKind kind, FailureSource source,
                  std::string detail);
Event MakeSupervisorLost(ClockPoint at, std::string detail);

} // namespace htmlppt
```

Create `native/src/core/state_machine_types.cpp`:

```cpp
#include "core/state_machine_types.h"

#include <utility>

namespace htmlppt {

Event MakeSlideChanged(ClockPoint at, int old_slide, int new_slide) {
  Event event;
  event.kind = EventKind::SlideChanged;
  event.at = at;
  event.old_slide = old_slide;
  event.new_slide = new_slide;
  return event;
}

Event MakeShowStarted(ClockPoint at) {
  Event event;
  event.kind = EventKind::ShowStarted;
  event.at = at;
  return event;
}

Event MakeShowEnded(ClockPoint at) {
  Event event;
  event.kind = EventKind::ShowEnded;
  event.at = at;
  return event;
}

Event MakeCoverHold(ClockPoint at) {
  Event event;
  event.kind = EventKind::CoverHold;
  event.at = at;
  return event;
}

Event MakeCoverDone(ClockPoint at) {
  Event event;
  event.kind = EventKind::CoverDone;
  event.at = at;
  return event;
}

Event MakeEdgeReady(ClockPoint at) {
  Event event;
  event.kind = EventKind::EdgeReady;
  event.at = at;
  return event;
}

Event MakeEdgeClosed(ClockPoint at) {
  Event event;
  event.kind = EventKind::EdgeClosed;
  event.at = at;
  return event;
}

Event MakeReturnRequested(ClockPoint at) {
  Event event;
  event.kind = EventKind::ReturnRequested;
  event.at = at;
  return event;
}

Event MakeTimeout(ClockPoint at, TimeoutKind kind) {
  Event event;
  event.kind = EventKind::Timeout;
  event.at = at;
  event.timeout = kind;
  return event;
}

Event MakeStopRequested(ClockPoint at, std::string source) {
  Event event;
  event.kind = EventKind::StopRequested;
  event.at = at;
  event.detail = std::move(source);
  return event;
}

Event MakeCleanupDone(ClockPoint at) {
  Event event;
  event.kind = EventKind::CleanupDone;
  event.at = at;
  return event;
}

Event MakePowerResume(ClockPoint at) {
  Event event;
  event.kind = EventKind::PowerResume;
  event.at = at;
  return event;
}

Event MakeFailure(ClockPoint at, FailureKind kind, FailureSource source,
                  std::string detail) {
  Event event;
  event.kind = EventKind::Failure;
  event.at = at;
  event.failure = kind;
  event.failure_source = source;
  event.detail = std::move(detail);
  return event;
}

Event MakeSupervisorLost(ClockPoint at, std::string detail) {
  Event event;
  event.kind = EventKind::SupervisorLost;
  event.at = at;
  event.detail = std::move(detail);
  return event;
}

} // namespace htmlppt
```

- [ ] **Step 4: Add the failure-policy primitives**

Create `native/src/core/failure_policy.h`:

```cpp
#pragma once

#include <optional>

#include "core/clock.h"
#include "core/types.h"

namespace htmlppt {

// Maps a failure observed in `state` to the category that owns it once
// transient retries are exhausted.
FailureKind EscalateFailure(State state);

// Tracks the first contiguous COM error. The window expires when COM errors
// have continued for at least the configured duration. A successful COM read
// must call reset().
class ComErrorWindow {
 public:
  explicit ComErrorWindow(Duration window);

  void record(ClockPoint now);
  void reset();
  bool expired(ClockPoint now) const;
  bool active() const;

 private:
  Duration window_;
  std::optional<ClockPoint> first_error_;
};

} // namespace htmlppt
```

Create `native/src/core/failure_policy.cpp`:

```cpp
#include "core/failure_policy.h"

namespace htmlppt {

FailureKind EscalateFailure(State state) {
  switch (state) {
    case State::CoverIn:
    case State::EdgeStarting:
    case State::ShowExit:
      return FailureKind::Switch;
    case State::Live:
    case State::Returning:
    case State::ReturnCoverOut:
      return FailureKind::Return;
    case State::Boot:
    case State::Warming:
    case State::Monitoring:
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
    case State::DegradedNoSupervisor:
      return FailureKind::Structural;
  }
  return FailureKind::Structural;
}

ComErrorWindow::ComErrorWindow(Duration window) : window_(window) {}

void ComErrorWindow::record(ClockPoint now) {
  if (!first_error_.has_value()) first_error_ = now;
}

void ComErrorWindow::reset() { first_error_.reset(); }

bool ComErrorWindow::expired(ClockPoint now) const {
  return first_error_.has_value() && now - *first_error_ >= window_;
}

bool ComErrorWindow::active() const { return first_error_.has_value(); }

} // namespace htmlppt
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 103/103 unit cases pass, CTest 8/8, no warnings.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/state_machine_types.h \
  native/src/core/state_machine_types.cpp \
  native/src/core/failure_policy.h native/src/core/failure_policy.cpp \
  native/tests/unit/failure_policy_test.cpp
git commit -m "feat: add state machine types and failure policy primitives"
```

---

### Task 2: State Machine, Timers, and Failure Outcomes

**Files:**
- Create: `native/src/core/state_machine.h`
- Create: `native/src/core/state_machine.cpp`
- Create: `native/tests/unit/state_machine_test.cpp`

**Interfaces:**
- Consumes: everything from Task 1; `Clock`, `RetryPolicy`, `ComErrorWindow`.
- Produces:
  - `class StateMachine` with:
    - `StateMachine(RunPlan plan, StateMachineConfig config, Clock& clock)`
    - `std::vector<Action> Start()`
    - `std::vector<Action> Handle(const Event& event)`
    - `std::vector<Action> Tick()`
    - `std::optional<ClockPoint> NextDeadline() const`
    - `StateSnapshot Snapshot() const`
    - `State state() const`
    - `bool IsDone(int slide) const`

- [ ] **Step 1: Write the failing state-machine tests**

Create `native/tests/unit/state_machine_test.cpp`:

```cpp
#include "test_main.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/state_machine.h"

using htmlppt::Action;
using htmlppt::ActionKind;
using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::FailureKind;
using htmlppt::FailureSource;
using htmlppt::MakeCleanupDone;
using htmlppt::MakeCoverDone;
using htmlppt::MakeCoverHold;
using htmlppt::MakeEdgeClosed;
using htmlppt::MakeEdgeReady;
using htmlppt::MakeFailure;
using htmlppt::MakeReturnRequested;
using htmlppt::MakeShowEnded;
using htmlppt::MakeShowStarted;
using htmlppt::MakeSlideChanged;
using htmlppt::MakeStopRequested;
using htmlppt::MakeSupervisorLost;
using htmlppt::OnFailurePolicy;
using htmlppt::RunPlan;
using htmlppt::State;
using htmlppt::StateMachine;
using htmlppt::StateMachineConfig;
using htmlppt::TriggerMode;

namespace {

bool HasAction(const std::vector<Action>& actions, ActionKind kind) {
  return std::any_of(actions.begin(), actions.end(),
                     [kind](const Action& action) {
                       return action.kind == kind;
                     });
}

const Action* FindAction(const std::vector<Action>& actions, ActionKind kind) {
  for (const auto& action : actions) {
    if (action.kind == kind) return &action;
  }
  return nullptr;
}

bool HasJournalContaining(const std::vector<Action>& actions,
                          std::string_view text) {
  return std::any_of(actions.begin(), actions.end(),
                     [text](const Action& action) {
                       return action.kind == ActionKind::Journal &&
                              action.detail.find(text) != std::string::npos;
                     });
}

RunPlan MakePlan() {
  RunPlan plan;
  plan.run_id = "test-run";
  plan.slide_count = 10;
  plan.targets = {
      {5, "Audit", "tef-live-audit"},
      {8, "Close", "tef-live-close"},
  };
  return plan;
}

StateMachineConfig MakeConfig() {
  StateMachineConfig config;
  config.startup_ignore = Duration{0};
  return config;
}

struct Machine {
  FakeClock clock;
  RunPlan plan;
  StateMachineConfig config;
  std::unique_ptr<StateMachine> machine;

  explicit Machine(StateMachineConfig config_in = MakeConfig(),
                   RunPlan plan_in = MakePlan())
      : plan(std::move(plan_in)), config(std::move(config_in)) {
    machine = std::make_unique<StateMachine>(plan, config, clock);
    machine->Start();
    machine->Handle(MakeShowStarted(clock.now()));
  }

  std::vector<Action> Slide(int old_slide, int new_slide) {
    return machine->Handle(MakeSlideChanged(clock.now(), old_slide, new_slide));
  }

  std::vector<Action> TriggerTargetFive() {
    Slide(4, 5);
    return Slide(5, 6);
  }

  void Advance(int ms) { clock.advance(Duration{ms}); }

  void AdvanceToNextDeadline() {
    const auto deadline = machine->NextDeadline();
    HTMLPPT_CHECK(deadline.has_value());
    clock.advance(std::chrono::duration_cast<Duration>(*deadline -
                                                       clock.now()));
  }

  void ReachLive() {
    TriggerTargetFive();
    machine->Handle(MakeCoverHold(clock.now()));
    machine->Handle(MakeEdgeReady(clock.now()));
    machine->Handle(MakeShowEnded(clock.now()));
  }

  void ReturnToMonitoring() {
    machine->Handle(MakeReturnRequested(clock.now()));
    machine->Handle(MakeCoverHold(clock.now()));
    machine->Handle(MakeCoverDone(clock.now()));
  }
};

} // namespace

HTMLPPT_TEST_CASE(state_machine_starts_in_warming_and_show_started_enters_monitoring) {
  FakeClock clock;
  StateMachine machine(MakePlan(), MakeConfig(), clock);
  HTMLPPT_CHECK(machine.state() == State::Boot);
  const auto started = machine.Start();
  HTMLPPT_CHECK(machine.state() == State::Warming);
  HTMLPPT_CHECK(HasJournalContaining(started, "state=Warming"));
  const auto shown = machine.Handle(MakeShowStarted(clock.now()));
  HTMLPPT_CHECK(machine.state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(shown, ActionKind::FocusWps));
  HTMLPPT_CHECK(machine.Snapshot().show_active);
}

HTMLPPT_TEST_CASE(state_machine_startup_ignore_window_suppresses_trigger) {
  FakeClock clock;
  auto config = MakeConfig();
  config.startup_ignore = Duration{1000};
  StateMachine machine(MakePlan(), config, clock);
  machine.Start();
  machine.Handle(MakeShowStarted(clock.now()));
  clock.advance(Duration{500});
  auto ignored = machine.Handle(MakeSlideChanged(clock.now(), 4, 5));
  HTMLPPT_CHECK(!HasAction(ignored, ActionKind::CoverFadeIn));
  clock.advance(Duration{400});
  ignored = machine.Handle(MakeSlideChanged(clock.now(), 5, 6));
  HTMLPPT_CHECK(!HasAction(ignored, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(machine.state() == State::Monitoring);
  clock.advance(Duration{200});
  machine.Handle(MakeSlideChanged(clock.now(), 4, 5));
  const auto triggered = machine.Handle(MakeSlideChanged(clock.now(), 5, 6));
  HTMLPPT_CHECK(HasAction(triggered, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(machine.state() == State::CoverIn);
}

HTMLPPT_TEST_CASE(state_machine_forward_leave_from_target_triggers) {
  Machine m;
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(m.machine->state() == State::CoverIn);
  HTMLPPT_CHECK(HasJournalContaining(actions, "trigger slide 5 -> Audit"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
  const auto snapshot = m.machine->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.current_target_slide.value_or(0), 5);
  HTMLPPT_CHECK(m.machine->IsDone(5));
  HTMLPPT_CHECK_EQ(snapshot.trigger_generation, std::uint64_t{1});
}

HTMLPPT_TEST_CASE(state_machine_backward_leave_does_not_trigger) {
  Machine m;
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 4);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
}

HTMLPPT_TEST_CASE(state_machine_arrive_trigger_fires_on_entry) {
  auto plan = MakePlan();
  plan.targets[0].trigger = TriggerMode::Arrive;
  Machine m(MakeConfig(), plan);
  const auto actions = m.Slide(4, 5);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->state() == State::CoverIn);
}

HTMLPPT_TEST_CASE(state_machine_cover_hold_launches_edge) {
  Machine m;
  m.TriggerTargetFive();
  const auto actions = m.machine->Handle(MakeCoverHold(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::EdgeStarting);
  const auto* launch = FindAction(actions, ActionKind::LaunchEdge);
  HTMLPPT_CHECK(launch != nullptr);
  HTMLPPT_CHECK_EQ(launch->slide, 5);
  HTMLPPT_CHECK_EQ(launch->detail, std::string{"tef-live-audit"});
}

HTMLPPT_TEST_CASE(state_machine_edge_ready_exits_show) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto actions = m.machine->Handle(MakeEdgeReady(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::ShowExit);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::ExitShow));
}

HTMLPPT_TEST_CASE(state_machine_show_ended_enters_live_and_sets_deadline) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  const auto actions = m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Live);
  HTMLPPT_CHECK(HasJournalContaining(actions, "live"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeOut));
  const auto deadline = m.machine->NextDeadline();
  HTMLPPT_CHECK(deadline.has_value());
  HTMLPPT_CHECK_EQ(
      std::chrono::duration_cast<Duration>(*deadline - m.clock.now()).count(),
      600000);
}

HTMLPPT_TEST_CASE(state_machine_return_requested_fades_and_resumes) {
  Machine m;
  m.ReachLive();
  const auto actions = m.machine->Handle(MakeReturnRequested(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasJournalContaining(actions, "return requested by page"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->Snapshot().return_prepared);
}

HTMLPPT_TEST_CASE(state_machine_cover_hold_in_returning_resumes_show) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto actions = m.machine->Handle(MakeCoverHold(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::ReturnCoverOut);
  const auto* resume = FindAction(actions, ActionKind::ResumeShow);
  HTMLPPT_CHECK(resume != nullptr);
  HTMLPPT_CHECK_EQ(resume->slide, 5);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::FocusWps));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeOut));
  HTMLPPT_CHECK(HasJournalContaining(actions, "resumed at slide 5"));
  std::size_t resume_index = actions.size();
  std::size_t fade_index = actions.size();
  for (std::size_t i = 0; i < actions.size(); ++i) {
    if (actions[i].kind == ActionKind::ResumeShow) resume_index = i;
    if (actions[i].kind == ActionKind::CoverFadeOut) fade_index = i;
  }
  HTMLPPT_CHECK(resume_index < fade_index);
}

HTMLPPT_TEST_CASE(state_machine_cover_done_returns_to_monitoring) {
  Machine m;
  m.ReachLive();
  m.ReturnToMonitoring();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  const auto snapshot = m.machine->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.prev_slide, -1);
  HTMLPPT_CHECK(!snapshot.current_target_slide.has_value());
  HTMLPPT_CHECK(snapshot.ignore_until.has_value());
  HTMLPPT_CHECK(!snapshot.return_prepared);
}

HTMLPPT_TEST_CASE(state_machine_edge_closed_without_signal_returns) {
  Machine m;
  m.ReachLive();
  const auto actions = m.machine->Handle(MakeEdgeClosed(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasJournalContaining(actions,
                                     "edge closed after return; cover fade out"));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_edge_live_timeout_kills_edge_and_returns) {
  Machine m;
  m.ReachLive();
  m.Advance(600001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_self_heals) {
  Machine m;
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::KillEdge));
  const auto* restore = FindAction(actions, ActionKind::RestoreShow);
  HTMLPPT_CHECK(restore != nullptr);
  HTMLPPT_CHECK_EQ(restore->slide, 5);
  HTMLPPT_CHECK(!m.machine->IsDone(5));
  HTMLPPT_CHECK_EQ(m.machine->Snapshot().consecutive_trigger_failures, 1);
  m.Advance(2001);
  m.Slide(4, 5);
  const auto retrigger = m.Slide(5, 6);
  HTMLPPT_CHECK(HasAction(retrigger, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_strict_keeps_done) {
  auto plan = MakePlan();
  plan.targets[0].on_failure = OnFailurePolicy::Strict;
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(m.machine->IsDone(5));
  m.Advance(2001);
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_safe_enters_safe_mode) {
  auto plan = MakePlan();
  plan.targets[0].on_failure = OnFailurePolicy::Safe;
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_cover_timeout_legacy_emits_fallback) {
  auto plan = MakePlan();
  plan.targets[0].on_failure = OnFailurePolicy::Legacy;
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::LegacyFallback));
}

HTMLPPT_TEST_CASE(state_machine_consecutive_switch_failures_enter_safe_mode) {
  auto config = MakeConfig();
  config.max_consecutive_trigger_failures = 2;
  Machine m(config);
  m.TriggerTargetFive();
  m.Advance(10001);
  m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  m.Advance(2001);
  m.Slide(4, 5);
  m.Slide(5, 6);
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_edge_ready_timeout_is_switch_failure) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.Advance(20001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_transient_failure_retries_after_backoff) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  const auto failed = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "exit failed"));
  HTMLPPT_CHECK(HasJournalContaining(failed, "retry in"));
  HTMLPPT_CHECK(m.machine->NextDeadline().has_value());
  m.AdvanceToNextDeadline();
  const auto retried = m.machine->Tick();
  HTMLPPT_CHECK(HasAction(retried, ActionKind::ExitShow));
  m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Live);
}

HTMLPPT_TEST_CASE(state_machine_launch_retry_kills_edge_first) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto failed = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Edge,
                  "launch failed"));
  HTMLPPT_CHECK(HasJournalContaining(failed, "retry in"));
  m.AdvanceToNextDeadline();
  const auto retried = m.machine->Tick();
  HTMLPPT_CHECK(HasAction(retried, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(retried, ActionKind::LaunchEdge));
  std::size_t kill_index = retried.size();
  std::size_t launch_index = retried.size();
  for (std::size_t i = 0; i < retried.size(); ++i) {
    if (retried[i].kind == ActionKind::KillEdge) kill_index = i;
    if (retried[i].kind == ActionKind::LaunchEdge) launch_index = i;
  }
  HTMLPPT_CHECK(kill_index < launch_index);
}

HTMLPPT_TEST_CASE(state_machine_retries_exhausted_escalate_to_switch_failure) {
  auto config = MakeConfig();
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Transient,
                                FailureSource::Other, "exit failed"));
  m.AdvanceToNextDeadline();
  m.machine->Tick();
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "exit failed again"));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_com_error_window_escalates_to_structural) {
  auto config = MakeConfig();
  config.com_error_window = Duration{2000};
  config.retry.max_attempts = 10;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto first = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(!HasAction(first, ActionKind::EnterSafeMode));
  m.Advance(2000);
  const auto second = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(second, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_return_failure_enters_faulted_and_keeps_edge) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Return, FailureSource::Other,
                  "resume failed"));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::KillEdge));
  HTMLPPT_CHECK(m.machine->Snapshot().return_prepared);
}

HTMLPPT_TEST_CASE(state_machine_faulted_edge_closed_only_journals) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Return,
                                FailureSource::Other, "resume failed"));
  const auto actions = m.machine->Handle(MakeEdgeClosed(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Journal));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_faulted_return_request_retries_return) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Return,
                                FailureSource::Other, "resume failed"));
  const auto actions = m.machine->Handle(MakeReturnRequested(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Returning);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_structural_failure_enters_safe_mode) {
  Machine m;
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Structural, FailureSource::Other,
                  "config missing"));
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_stop_requested_cleans_up_and_exits) {
  Machine m;
  m.ReachLive();
  const auto stopping =
      m.machine->Handle(MakeStopRequested(m.clock.now(), "test"));
  HTMLPPT_CHECK(m.machine->state() == State::Stopping);
  HTMLPPT_CHECK(HasJournalContaining(stopping, "cleanup start"));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::KillEdge));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::StopHelperThreads));
  HTMLPPT_CHECK(HasAction(stopping, ActionKind::ReleaseLock));
  const auto done = m.machine->Handle(MakeCleanupDone(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Exited);
  HTMLPPT_CHECK(HasJournalContaining(done, "cleanup done"));
}

HTMLPPT_TEST_CASE(state_machine_supervisor_loss_continue_sets_degraded) {
  Machine m;
  m.ReachLive();
  const auto actions = m.machine->Handle(
      MakeSupervisorLost(m.clock.now(), "pipe closed"));
  HTMLPPT_CHECK(m.machine->state() == State::Live);
  HTMLPPT_CHECK(m.machine->Snapshot().degraded_no_supervisor);
  HTMLPPT_CHECK(HasJournalContaining(actions, "supervisor lost"));
}

HTMLPPT_TEST_CASE(state_machine_supervisor_loss_cleanup_exits) {
  auto config = MakeConfig();
  config.supervisor_loss = htmlppt::SupervisorLossPolicy::Cleanup;
  Machine m(config);
  m.ReachLive();
  const auto actions = m.machine->Handle(
      MakeSupervisorLost(m.clock.now(), "pipe closed"));
  HTMLPPT_CHECK(m.machine->state() == State::Stopping);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  m.machine->Handle(MakeCleanupDone(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Exited);
}

HTMLPPT_TEST_CASE(state_machine_cover_done_timeout_hides_cover_and_monitors) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  const auto* hide = FindAction(actions, ActionKind::CoverHide);
  HTMLPPT_CHECK(hide != nullptr);
  HTMLPPT_CHECK_EQ(hide->slide, 5);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
}

HTMLPPT_TEST_CASE(state_machine_show_ended_resets_done_flags) {
  Machine m;
  m.ReachLive();
  m.ReturnToMonitoring();
  m.Advance(2001);
  HTMLPPT_CHECK(m.machine->IsDone(5));
  m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(!m.machine->IsDone(5));
  m.machine->Handle(MakeShowStarted(m.clock.now()));
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverFadeIn));
}

HTMLPPT_TEST_CASE(state_machine_once_per_show) {
  Machine m;
  m.ReachLive();
  m.ReturnToMonitoring();
  m.Advance(2001);
  m.Slide(4, 5);
  const auto actions = m.Slide(5, 6);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::CoverFadeIn));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
}

HTMLPPT_TEST_CASE(state_machine_snapshot_reports_guards) {
  Machine m;
  m.TriggerTargetFive();
  const auto snapshot = m.machine->Snapshot();
  HTMLPPT_CHECK(snapshot.state == State::CoverIn);
  HTMLPPT_CHECK_EQ(snapshot.prev_slide, 6);
  HTMLPPT_CHECK_EQ(snapshot.current_target_slide.value_or(0), 5);
  HTMLPPT_CHECK(snapshot.show_active);
  HTMLPPT_CHECK(m.machine->IsDone(5));
}

HTMLPPT_TEST_CASE(state_machine_return_resume_timeout_enters_faulted) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::KillEdge));
}

HTMLPPT_TEST_CASE(state_machine_returning_transient_failure_retries_then_faults) {
  auto config = MakeConfig();
  config.retry.max_attempts = 1;
  config.retry.jitter_ratio = 0.0;
  Machine m(config);
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto failed = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "cover failed"));
  HTMLPPT_CHECK(HasJournalContaining(failed, "retry in"));
  m.AdvanceToNextDeadline();
  const auto retried = m.machine->Tick();
  HTMLPPT_CHECK(HasAction(retried, ActionKind::CoverFadeIn));
  const auto exhausted = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Other,
                  "cover failed again"));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(exhausted, ActionKind::CoverHide));
}

HTMLPPT_TEST_CASE(state_machine_show_ended_during_return_enters_faulted) {
  Machine m;
  m.ReachLive();
  m.machine->Handle(MakeReturnRequested(m.clock.now()));
  const auto actions = m.machine->Handle(MakeShowEnded(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Faulted);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::Diagnostic));
}

HTMLPPT_TEST_CASE(state_machine_edge_closed_before_ready_is_switch_failure) {
  Machine m;
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  const auto actions = m.machine->Handle(MakeEdgeClosed(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::CoverHide));
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_com_success_resets_window) {
  auto config = MakeConfig();
  config.com_error_window = Duration{2000};
  config.retry.max_attempts = 10;
  Machine m(config);
  m.TriggerTargetFive();
  m.machine->Handle(MakeCoverHold(m.clock.now()));
  m.machine->Handle(MakeFailure(m.clock.now(), FailureKind::Transient,
                                FailureSource::Com, "COM busy"));
  m.machine->Handle(MakeEdgeReady(m.clock.now()));
  m.Advance(2000);
  const auto actions = m.machine->Handle(
      MakeFailure(m.clock.now(), FailureKind::Transient, FailureSource::Com,
                  "COM busy"));
  HTMLPPT_CHECK(m.machine->state() != State::SafeMode);
  HTMLPPT_CHECK(!HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_per_target_cover_max_hold_override) {
  auto plan = MakePlan();
  plan.targets[0].cover_max_hold = Duration{5000};
  Machine m(MakeConfig(), plan);
  m.TriggerTargetFive();
  m.Advance(5001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::Monitoring);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::RestoreShow));
}

HTMLPPT_TEST_CASE(state_machine_per_target_failure_threshold_override) {
  auto config = MakeConfig();
  config.max_consecutive_trigger_failures = 5;
  auto plan = MakePlan();
  plan.targets[0].max_consecutive_trigger_failures = 1;
  Machine m(config, plan);
  m.TriggerTargetFive();
  m.Advance(10001);
  const auto actions = m.machine->Tick();
  HTMLPPT_CHECK(m.machine->state() == State::SafeMode);
  HTMLPPT_CHECK(HasAction(actions, ActionKind::EnterSafeMode));
}

HTMLPPT_TEST_CASE(state_machine_events_after_exit_are_ignored) {
  Machine m;
  m.machine->Handle(MakeStopRequested(m.clock.now(), "test"));
  m.machine->Handle(MakeCleanupDone(m.clock.now()));
  HTMLPPT_CHECK(m.machine->state() == State::Exited);
  HTMLPPT_CHECK(m.Slide(1, 2).empty());
  HTMLPPT_CHECK(m.machine->Handle(MakeShowStarted(m.clock.now())).empty());
}
```

- [ ] **Step 2: Run the tests and verify they fail**

```bash
cmake --build native/build --parallel
```

Expected: compilation fails because `core/state_machine.h` does not exist.

- [ ] **Step 3: Add the state-machine header**

Create `native/src/core/state_machine.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/failure_policy.h"
#include "core/retry_policy.h"
#include "core/state_machine_types.h"

namespace htmlppt {

class StateMachine {
 public:
  StateMachine(RunPlan plan, StateMachineConfig config, Clock& clock);

  std::vector<Action> Start();
  std::vector<Action> Handle(const Event& event);
  std::vector<Action> Tick();
  std::optional<ClockPoint> NextDeadline() const;
  StateSnapshot Snapshot() const;
  State state() const { return state_; }
  bool IsDone(int slide) const { return done_slides_.count(slide) != 0; }

 private:
  enum class RetryOperation {
    None,
    CoverFadeIn,
    LaunchEdge,
    ExitShow,
  };

  const TargetPlan* FindTarget(int slide) const;
  int EffectiveFailureThreshold(const TargetPlan& target) const;
  Duration EffectiveCoverMaxHold(const TargetPlan& target) const;
  void Enter(State state);
  void ResetRetries();
  void ClearDeadlines();
  void SetDeadline(TimeoutKind kind, ClockPoint at);
  void ClearDeadline(TimeoutKind kind);
  std::optional<ClockPoint> Deadline(TimeoutKind kind) const;

  std::vector<Action> OnSlideChanged(const Event& event);
  std::vector<Action> OnShowStarted(const Event& event);
  std::vector<Action> OnShowEnded(const Event& event);
  std::vector<Action> OnCoverHold(const Event& event);
  std::vector<Action> OnCoverDone(const Event& event);
  std::vector<Action> OnEdgeReady(const Event& event);
  std::vector<Action> OnEdgeClosed(const Event& event);
  std::vector<Action> OnReturnRequested(const Event& event);
  std::vector<Action> OnTimeout(const Event& event);
  std::vector<Action> OnStopRequested(const Event& event);
  std::vector<Action> OnCleanupDone(const Event& event);
  std::vector<Action> OnFailure(const Event& event);
  std::vector<Action> OnSupervisorLost(const Event& event);

  std::vector<Action> BeginTrigger(const TargetPlan& target, ClockPoint at);
  std::vector<Action> BeginReturn(ClockPoint at, bool prepared,
                                  std::string detail);
  std::vector<Action> BeginRetry(RetryOperation operation, ClockPoint at,
                                 std::string detail);
  std::vector<Action> ExecuteRetry(ClockPoint at);
  std::vector<Action> HandleSwitchFailure(ClockPoint at, std::string detail);
  std::vector<Action> HandleReturnFailure(std::string detail);
  std::vector<Action> HandleStructuralFailure(std::string detail);
  std::vector<Action> BeginCleanup(std::string detail);
  std::vector<Action> Journal(std::string detail, int slide = 0) const;

  RunPlan plan_;
  StateMachineConfig config_;
  Clock& clock_;
  RetryPolicy retry_;
  ComErrorWindow com_window_;

  State state_ = State::Boot;
  int prev_slide_ = -1;
  std::set<int> done_slides_;
  std::optional<int> current_target_slide_;
  // Diagnostic/status only. Duplicate-resume prevention is enforced by the
  // Returning/ReturnCoverOut state gates, not by this flag.
  bool return_prepared_ = false;
  bool degraded_ = false;
  bool safe_mode_ = false;
  bool show_active_ = false;
  int consecutive_trigger_failures_ = 0;
  std::uint64_t trigger_generation_ = 0;
  std::optional<ClockPoint> ignore_until_;
  std::optional<ClockPoint> cover_hold_deadline_;
  std::optional<ClockPoint> edge_ready_deadline_;
  std::optional<ClockPoint> edge_live_deadline_;
  std::optional<ClockPoint> return_resume_deadline_;
  std::optional<ClockPoint> cover_done_deadline_;
  std::optional<ClockPoint> retry_deadline_;
  RetryOperation pending_retry_ = RetryOperation::None;
  int retry_attempt_ = 0;
};

} // namespace htmlppt
```

- [ ] **Step 4: Add the state-machine implementation**

Create `native/src/core/state_machine.cpp`:

```cpp
#include "core/state_machine.h"

#include <utility>

namespace htmlppt {

StateMachine::StateMachine(RunPlan plan, StateMachineConfig config,
                           Clock& clock)
    : plan_(std::move(plan)),
      config_(std::move(config)),
      clock_(clock),
      retry_(config_.retry),
      com_window_(config_.com_error_window) {}

std::vector<Action> StateMachine::Start() {
  if (state_ != State::Boot) return {};
  Enter(State::Warming);
  ignore_until_ = clock_.now() + config_.startup_ignore;
  return Journal("state=Warming");
}

std::vector<Action> StateMachine::Handle(const Event& event) {
  if (state_ == State::Exited) return {};
  switch (event.kind) {
    case EventKind::SlideChanged:
      return OnSlideChanged(event);
    case EventKind::ShowStarted:
      return OnShowStarted(event);
    case EventKind::ShowEnded:
      return OnShowEnded(event);
    case EventKind::CoverHold:
      return OnCoverHold(event);
    case EventKind::CoverDone:
      return OnCoverDone(event);
    case EventKind::EdgeReady:
      return OnEdgeReady(event);
    case EventKind::EdgeClosed:
      return OnEdgeClosed(event);
    case EventKind::ReturnRequested:
      return OnReturnRequested(event);
    case EventKind::Timeout:
      return OnTimeout(event);
    case EventKind::StopRequested:
      return OnStopRequested(event);
    case EventKind::CleanupDone:
      return OnCleanupDone(event);
    case EventKind::PowerResume:
      return Journal("power resume");
    case EventKind::ConfigReloaded:
      return Journal("config reload ignored");
    case EventKind::Failure:
      return OnFailure(event);
    case EventKind::SupervisorLost:
      return OnSupervisorLost(event);
  }
  return {};
}

std::vector<Action> StateMachine::Tick() {
  const auto now = clock_.now();
  const auto earliest = NextDeadline();
  if (!earliest.has_value() || now < *earliest) return {};
  const TimeoutKind order[] = {
      TimeoutKind::CoverHold, TimeoutKind::EdgeReady, TimeoutKind::EdgeLive,
      TimeoutKind::ReturnResume, TimeoutKind::CoverDone, TimeoutKind::Retry};
  for (const auto kind : order) {
    const auto deadline = Deadline(kind);
    if (deadline.has_value() && *deadline <= now) {
      return Handle(MakeTimeout(now, kind));
    }
  }
  return {};
}

std::optional<ClockPoint> StateMachine::NextDeadline() const {
  const std::optional<ClockPoint>* deadlines[] = {
      &cover_hold_deadline_, &edge_ready_deadline_, &edge_live_deadline_,
      &return_resume_deadline_, &cover_done_deadline_, &retry_deadline_};
  std::optional<ClockPoint> earliest;
  for (const auto* deadline : deadlines) {
    if (deadline->has_value() &&
        (!earliest.has_value() || **deadline < *earliest)) {
      earliest = **deadline;
    }
  }
  return earliest;
}

StateSnapshot StateMachine::Snapshot() const {
  StateSnapshot snapshot;
  snapshot.state = state_;
  snapshot.prev_slide = prev_slide_;
  snapshot.current_target_slide = current_target_slide_;
  snapshot.consecutive_trigger_failures = consecutive_trigger_failures_;
  snapshot.trigger_generation = trigger_generation_;
  snapshot.return_prepared = return_prepared_;
  snapshot.degraded_no_supervisor = degraded_;
  snapshot.safe_mode = safe_mode_;
  snapshot.show_active = show_active_;
  snapshot.ignore_until = ignore_until_;
  return snapshot;
}

const TargetPlan* StateMachine::FindTarget(int slide) const {
  for (const auto& target : plan_.targets) {
    if (target.slide == slide) return &target;
  }
  return nullptr;
}

int StateMachine::EffectiveFailureThreshold(const TargetPlan& target) const {
  return target.max_consecutive_trigger_failures.value_or(
      config_.max_consecutive_trigger_failures);
}

Duration StateMachine::EffectiveCoverMaxHold(const TargetPlan& target) const {
  return target.cover_max_hold.value_or(config_.cover_max_hold);
}

void StateMachine::Enter(State state) {
  state_ = state;
  if (state == State::SafeMode) safe_mode_ = true;
}

void StateMachine::ResetRetries() {
  retry_.reset();
  retry_attempt_ = 0;
  pending_retry_ = RetryOperation::None;
  retry_deadline_.reset();
}

void StateMachine::ClearDeadlines() {
  cover_hold_deadline_.reset();
  edge_ready_deadline_.reset();
  edge_live_deadline_.reset();
  return_resume_deadline_.reset();
  cover_done_deadline_.reset();
  retry_deadline_.reset();
}

void StateMachine::SetDeadline(TimeoutKind kind, ClockPoint at) {
  switch (kind) {
    case TimeoutKind::CoverHold:
      cover_hold_deadline_ = at;
      break;
    case TimeoutKind::EdgeReady:
      edge_ready_deadline_ = at;
      break;
    case TimeoutKind::EdgeLive:
      edge_live_deadline_ = at;
      break;
    case TimeoutKind::ReturnResume:
      return_resume_deadline_ = at;
      break;
    case TimeoutKind::CoverDone:
      cover_done_deadline_ = at;
      break;
    case TimeoutKind::Retry:
      retry_deadline_ = at;
      break;
  }
}

void StateMachine::ClearDeadline(TimeoutKind kind) {
  switch (kind) {
    case TimeoutKind::CoverHold:
      cover_hold_deadline_.reset();
      break;
    case TimeoutKind::EdgeReady:
      edge_ready_deadline_.reset();
      break;
    case TimeoutKind::EdgeLive:
      edge_live_deadline_.reset();
      break;
    case TimeoutKind::ReturnResume:
      return_resume_deadline_.reset();
      break;
    case TimeoutKind::CoverDone:
      cover_done_deadline_.reset();
      break;
    case TimeoutKind::Retry:
      retry_deadline_.reset();
      break;
  }
}

std::optional<ClockPoint> StateMachine::Deadline(TimeoutKind kind) const {
  switch (kind) {
    case TimeoutKind::CoverHold:
      return cover_hold_deadline_;
    case TimeoutKind::EdgeReady:
      return edge_ready_deadline_;
    case TimeoutKind::EdgeLive:
      return edge_live_deadline_;
    case TimeoutKind::ReturnResume:
      return return_resume_deadline_;
    case TimeoutKind::CoverDone:
      return cover_done_deadline_;
    case TimeoutKind::Retry:
      return retry_deadline_;
  }
  return std::nullopt;
}

std::vector<Action> StateMachine::OnSlideChanged(const Event& event) {
  if (state_ != State::Monitoring) return {};
  com_window_.reset();
  ResetRetries();
  const int previous = prev_slide_ >= 0 ? prev_slide_ : event.old_slide;
  prev_slide_ = event.new_slide;
  if (ignore_until_.has_value() && event.at < *ignore_until_) return {};
  for (const auto& target : plan_.targets) {
    if (!target.enabled || done_slides_.count(target.slide) != 0) continue;
    const bool matches =
        target.trigger == TriggerMode::Arrive
            ? target.slide == event.new_slide && event.new_slide != previous
            : target.slide == previous && event.new_slide > previous;
    if (matches) return BeginTrigger(target, event.at);
  }
  return {};
}

std::vector<Action> StateMachine::OnShowStarted(const Event& event) {
  if (state_ != State::Warming && state_ != State::Monitoring) return {};
  const bool restarted = state_ == State::Monitoring;
  done_slides_.clear();
  prev_slide_ = -1;
  current_target_slide_.reset();
  return_prepared_ = false;
  consecutive_trigger_failures_ = 0;
  ignore_until_ = event.at + config_.startup_ignore;
  ResetRetries();
  ClearDeadlines();
  com_window_.reset();
  show_active_ = true;
  Enter(State::Monitoring);
  auto actions = Journal(restarted ? "show restarted" : "show started");
  actions.push_back({ActionKind::FocusWps, 0, "focus WPS"});
  return actions;
}

std::vector<Action> StateMachine::OnShowEnded(const Event& event) {
  if (state_ == State::ShowExit) {
    ClearDeadline(TimeoutKind::EdgeReady);
    ResetRetries();
    com_window_.reset();
    Enter(State::Live);
    show_active_ = false;
    consecutive_trigger_failures_ = 0;
    SetDeadline(TimeoutKind::EdgeLive, event.at + config_.edge_max_live);
    auto actions = Journal("live", current_target_slide_.value_or(0));
    actions.push_back({ActionKind::CoverFadeOut,
                       current_target_slide_.value_or(0), "reveal Edge"});
    return actions;
  }
  if (state_ == State::Monitoring) {
    show_active_ = false;
    done_slides_.clear();
    prev_slide_ = -1;
    current_target_slide_.reset();
    return Journal("show ended");
  }
  if (state_ == State::Returning || state_ == State::ReturnCoverOut) {
    return HandleReturnFailure("show ended during return");
  }
  if (state_ == State::Live) {
    return Journal("show ended while live");
  }
  return {};
}

std::vector<Action> StateMachine::OnCoverHold(const Event& event) {
  if (state_ == State::CoverIn) {
    ClearDeadline(TimeoutKind::CoverHold);
    ResetRetries();
    com_window_.reset();
    Enter(State::EdgeStarting);
    SetDeadline(TimeoutKind::EdgeReady, event.at + config_.edge_ready_timeout);
    const int slide = current_target_slide_.value_or(0);
    const TargetPlan* target = FindTarget(slide);
    const std::string profile =
        target ? target->edge_profile : std::string{};
    auto actions = Journal("cover hold", slide);
    actions.push_back({ActionKind::LaunchEdge, slide, profile});
    return actions;
  }
  if (state_ == State::Returning) {
    ClearDeadline(TimeoutKind::ReturnResume);
    ResetRetries();
    com_window_.reset();
    const int slide = current_target_slide_.value_or(0);
    Enter(State::ReturnCoverOut);
    show_active_ = true;
    SetDeadline(TimeoutKind::CoverDone, event.at + config_.cover_max_hold);
    auto actions = Journal("resumed at slide " + std::to_string(slide), slide);
    actions.push_back({ActionKind::ResumeShow, slide, "resume WPS"});
    actions.push_back({ActionKind::FocusWps, slide, "focus WPS"});
    actions.push_back({ActionKind::CoverFadeOut, slide, "fade out cover"});
    return actions;
  }
  return {};
}

std::vector<Action> StateMachine::OnCoverDone(const Event& event) {
  if (state_ != State::ReturnCoverOut) return {};
  ClearDeadline(TimeoutKind::CoverDone);
  ResetRetries();
  Enter(State::Monitoring);
  ignore_until_ = event.at + config_.return_ignore;
  prev_slide_ = -1;
  current_target_slide_.reset();
  return_prepared_ = false;
  return Journal("return cover done");
}

std::vector<Action> StateMachine::OnEdgeReady(const Event& event) {
  if (state_ != State::EdgeStarting) return {};
  ClearDeadline(TimeoutKind::EdgeReady);
  ResetRetries();
  com_window_.reset();
  const int slide = current_target_slide_.value_or(0);
  Enter(State::ShowExit);
  auto actions = Journal("edge ready", slide);
  actions.push_back({ActionKind::ExitShow, slide, "exit WPS show"});
  return actions;
}

std::vector<Action> StateMachine::OnEdgeClosed(const Event& event) {
  if (state_ == State::Live) {
    return BeginReturn(event.at, false,
                       "edge closed after return; cover fade out");
  }
  if (state_ == State::EdgeStarting || state_ == State::ShowExit) {
    return HandleSwitchFailure(event.at, "edge closed before ready");
  }
  if (state_ == State::Faulted) {
    return Journal("edge closed after return failure",
                   current_target_slide_.value_or(0));
  }
  return {};
}

std::vector<Action> StateMachine::OnReturnRequested(const Event& event) {
  if (state_ == State::Live) {
    return BeginReturn(event.at, true, "return requested by page");
  }
  if (state_ == State::Faulted) {
    return BeginReturn(event.at, true, "return requested by page (retry)");
  }
  return {};
}

std::vector<Action> StateMachine::OnTimeout(const Event& event) {
  switch (event.timeout) {
    case TimeoutKind::CoverHold:
      if (state_ == State::CoverIn) {
        ClearDeadline(TimeoutKind::CoverHold);
        return HandleSwitchFailure(event.at, "cover hold timeout");
      }
      break;
    case TimeoutKind::EdgeReady:
      if (state_ == State::EdgeStarting) {
        ClearDeadline(TimeoutKind::EdgeReady);
        return HandleSwitchFailure(event.at, "edge ready timeout");
      }
      break;
    case TimeoutKind::EdgeLive:
      if (state_ == State::Live) {
        ClearDeadline(TimeoutKind::EdgeLive);
        auto actions = Journal("edge live timeout",
                               current_target_slide_.value_or(0));
        actions.push_back({ActionKind::KillEdge,
                           current_target_slide_.value_or(0),
                           "edge live timeout"});
        auto returning = BeginReturn(event.at, false, "edge live timeout");
        actions.insert(actions.end(), returning.begin(), returning.end());
        return actions;
      }
      break;
    case TimeoutKind::ReturnResume:
      if (state_ == State::Returning) {
        ClearDeadline(TimeoutKind::ReturnResume);
        return HandleReturnFailure("resume show timeout");
      }
      break;
    case TimeoutKind::CoverDone:
      if (state_ == State::ReturnCoverOut) {
        ClearDeadline(TimeoutKind::CoverDone);
        const int slide = current_target_slide_.value_or(0);
        Enter(State::Monitoring);
        ignore_until_ = event.at + config_.return_ignore;
        prev_slide_ = -1;
        current_target_slide_.reset();
        return_prepared_ = false;
        auto actions = Journal("cover done timeout; hiding cover", slide);
        actions.push_back(
            {ActionKind::CoverHide, slide, "cover done timeout"});
        actions.push_back(
            {ActionKind::Diagnostic, slide, "cover fade-out timed out"});
        return actions;
      }
      break;
    case TimeoutKind::Retry:
      return ExecuteRetry(event.at);
  }
  return {};
}

std::vector<Action> StateMachine::OnStopRequested(const Event& event) {
  if (state_ == State::Stopping) return {};
  return BeginCleanup("stop requested: " + event.detail);
}

std::vector<Action> StateMachine::OnCleanupDone(const Event& event) {
  (void)event;
  if (state_ != State::Stopping) return {};
  Enter(State::Exited);
  return Journal("cleanup done");
}

std::vector<Action> StateMachine::OnFailure(const Event& event) {
  if (state_ == State::Stopping || state_ == State::Exited ||
      state_ == State::SafeMode) {
    return {};
  }
  if (state_ == State::Faulted) {
    return Journal("failure while faulted: " + event.detail);
  }
  if (event.failure == FailureKind::Structural) {
    return HandleStructuralFailure(event.detail);
  }
  if (event.failure == FailureKind::Switch) {
    return HandleSwitchFailure(event.at, event.detail);
  }
  if (event.failure == FailureKind::Return) {
    return HandleReturnFailure(event.detail);
  }
  if (event.failure_source == FailureSource::Com) {
    com_window_.record(event.at);
    if (com_window_.expired(event.at)) {
      return HandleStructuralFailure("COM busy: " + event.detail);
    }
  }
  RetryOperation operation = RetryOperation::None;
  switch (state_) {
    case State::CoverIn:
      operation = RetryOperation::CoverFadeIn;
      break;
    case State::EdgeStarting:
      operation = RetryOperation::LaunchEdge;
      break;
    case State::ShowExit:
      operation = RetryOperation::ExitShow;
      break;
    case State::Returning:
      operation = RetryOperation::CoverFadeIn;
      break;
    case State::ReturnCoverOut:
      return Journal("transient failure while returning: " + event.detail);
    case State::Live:
      return Journal("transient failure while live: " + event.detail);
    case State::Monitoring:
      return Journal("transient failure while monitoring: " + event.detail);
    case State::Boot:
    case State::Warming:
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
    case State::DegradedNoSupervisor:
      // Stopping/Faulted/SafeMode/Exited are handled by the early returns above;
      // the remaining states are structural failures.
      return HandleStructuralFailure(event.detail);
  }
  return BeginRetry(operation, event.at, event.detail);
}

std::vector<Action> StateMachine::OnSupervisorLost(const Event& event) {
  if (state_ == State::Stopping) return {};
  if (config_.supervisor_loss == SupervisorLossPolicy::Cleanup) {
    return BeginCleanup("supervisor lost: " + event.detail);
  }
  degraded_ = true;
  return Journal("supervisor lost; continuing: " + event.detail);
}

std::vector<Action> StateMachine::BeginTrigger(const TargetPlan& target,
                                               ClockPoint at) {
  done_slides_.insert(target.slide);
  current_target_slide_ = target.slide;
  ++trigger_generation_;
  ResetRetries();
  ClearDeadlines();
  Enter(State::CoverIn);
  SetDeadline(TimeoutKind::CoverHold,
              at + EffectiveCoverMaxHold(target));
  auto actions =
      Journal("trigger slide " + std::to_string(target.slide) + " -> " +
                  target.label,
              target.slide);
  actions.push_back({ActionKind::CoverFadeIn, target.slide, "cover fade in"});
  return actions;
}

std::vector<Action> StateMachine::BeginReturn(ClockPoint at, bool prepared,
                                              std::string detail) {
  const int slide = current_target_slide_.value_or(0);
  return_prepared_ = prepared;
  ResetRetries();
  ClearDeadlines();
  Enter(State::Returning);
  SetDeadline(TimeoutKind::ReturnResume, at + config_.cover_max_hold);
  auto actions = Journal(std::move(detail), slide);
  actions.push_back({ActionKind::CoverFadeIn, slide, "return cover fade"});
  return actions;
}

std::vector<Action> StateMachine::BeginRetry(RetryOperation operation,
                                             ClockPoint at,
                                             std::string detail) {
  const auto delay = retry_.next_delay(retry_attempt_);
  if (!delay.has_value()) {
    const FailureKind escalated = EscalateFailure(state_);
    if (escalated == FailureKind::Switch) {
      return HandleSwitchFailure(at, detail + " (retries exhausted)");
    }
    if (escalated == FailureKind::Return) {
      return HandleReturnFailure(detail + " (retries exhausted)");
    }
    return HandleStructuralFailure(detail + " (retries exhausted)");
  }
  ++retry_attempt_;
  pending_retry_ = operation;
  SetDeadline(TimeoutKind::Retry, at + *delay);
  auto actions = Journal("transient failure; retry in " +
                         std::to_string(delay->count()) + "ms: " + detail);
  actions.push_back(
      {ActionKind::Diagnostic, current_target_slide_.value_or(0), detail});
  return actions;
}

std::vector<Action> StateMachine::ExecuteRetry(ClockPoint at) {
  ClearDeadline(TimeoutKind::Retry);
  const RetryOperation operation = pending_retry_;
  pending_retry_ = RetryOperation::None;
  const int slide = current_target_slide_.value_or(0);
  switch (operation) {
    case RetryOperation::CoverFadeIn:
      return {{ActionKind::CoverFadeIn, slide, "retry cover fade"}};
    case RetryOperation::LaunchEdge: {
      SetDeadline(TimeoutKind::EdgeReady, at + config_.edge_ready_timeout);
      const TargetPlan* target = FindTarget(slide);
      return {{ActionKind::KillEdge, slide, "retry launch Edge"},
              {ActionKind::LaunchEdge, slide,
               target ? target->edge_profile : std::string{}}};
    }
    case RetryOperation::ExitShow:
      return {{ActionKind::ExitShow, slide, "retry exit show"}};
    case RetryOperation::None:
      return {};
  }
  return {};
}

std::vector<Action> StateMachine::HandleSwitchFailure(ClockPoint at,
                                                      std::string detail) {
  const int slide = current_target_slide_.value_or(0);
  const TargetPlan* target = FindTarget(slide);
  const OnFailurePolicy policy =
      target ? target->on_failure : config_.default_on_failure;
  const int threshold = target ? EffectiveFailureThreshold(*target)
                               : config_.max_consecutive_trigger_failures;

  ClearDeadlines();
  com_window_.reset();
  ResetRetries();
  ++consecutive_trigger_failures_;

  std::vector<Action> actions;
  actions.push_back({ActionKind::CoverHide, slide, "switch failure"});
  actions.push_back({ActionKind::KillEdge, slide, "switch failure"});
  actions.push_back(
      {ActionKind::Journal, slide, "switch failure: " + detail});

  if (policy == OnFailurePolicy::Safe ||
      policy == OnFailurePolicy::Legacy ||
      consecutive_trigger_failures_ >= threshold) {
    Enter(State::SafeMode);
    show_active_ = false;
    actions.push_back({ActionKind::EnterSafeMode, slide,
                       "switch failure threshold or policy"});
    if (policy == OnFailurePolicy::Legacy) {
      actions.push_back({ActionKind::LegacyFallback, slide, "switch failure"});
    }
    current_target_slide_.reset();
    return actions;
  }

  if (policy == OnFailurePolicy::SelfHeal && slide > 0) {
    done_slides_.erase(slide);
  }
  prev_slide_ = slide > 0 ? slide : -1;
  ignore_until_ = at + config_.return_ignore;
  show_active_ = true;
  Enter(State::Monitoring);
  actions.push_back(
      {ActionKind::RestoreShow, slide, "restore WPS after switch failure"});
  actions.push_back(
      {ActionKind::FocusWps, slide, "focus WPS after switch failure"});
  actions.push_back(
      {ActionKind::Journal, slide,
       "switch failure recovered; monitoring at slide " +
           std::to_string(slide)});
  current_target_slide_.reset();
  return actions;
}

std::vector<Action> StateMachine::HandleReturnFailure(std::string detail) {
  ClearDeadlines();
  ResetRetries();
  return_prepared_ = true;
  show_active_ = false;
  const int slide = current_target_slide_.value_or(0);
  Enter(State::Faulted);
  std::vector<Action> actions;
  actions.push_back({ActionKind::CoverHide, slide, "return failure"});
  actions.push_back(
      {ActionKind::Journal, slide, "return failure: " + detail});
  actions.push_back(
      {ActionKind::Diagnostic, slide,
       "WPS resume failed; close Edge manually or retry"});
  return actions;
}

std::vector<Action> StateMachine::HandleStructuralFailure(std::string detail) {
  ClearDeadlines();
  ResetRetries();
  const int slide = current_target_slide_.value_or(0);
  const TargetPlan* target = FindTarget(slide);
  const OnFailurePolicy policy =
      target ? target->on_failure : config_.default_on_failure;
  Enter(State::SafeMode);
  show_active_ = false;
  std::vector<Action> actions;
  actions.push_back({ActionKind::CoverHide, slide, "structural failure"});
  actions.push_back({ActionKind::KillEdge, slide, "structural failure"});
  actions.push_back(
      {ActionKind::Journal, slide, "structural failure: " + detail});
  actions.push_back(
      {ActionKind::EnterSafeMode, slide, "structural failure"});
  if (policy == OnFailurePolicy::Legacy) {
    actions.push_back(
        {ActionKind::LegacyFallback, slide, "structural failure"});
  }
  current_target_slide_.reset();
  return actions;
}

std::vector<Action> StateMachine::BeginCleanup(std::string detail) {
  ClearDeadlines();
  ResetRetries();
  const int slide = current_target_slide_.value_or(0);
  show_active_ = false;
  Enter(State::Stopping);
  std::vector<Action> actions;
  actions.push_back({ActionKind::Journal, slide, "cleanup start"});
  actions.push_back({ActionKind::CoverHide, slide, detail});
  actions.push_back({ActionKind::KillEdge, slide, detail});
  actions.push_back({ActionKind::StopHelperThreads, slide, detail});
  actions.push_back({ActionKind::ReleaseLock, slide, detail});
  current_target_slide_.reset();
  return actions;
}

std::vector<Action> StateMachine::Journal(std::string detail,
                                          int slide) const {
  return {{ActionKind::Journal, slide, std::move(detail)}};
}

} // namespace htmlppt
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 145/145 unit cases pass, CTest 8/8, no warnings.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/state_machine.h native/src/core/state_machine.cpp \
  native/tests/unit/state_machine_test.cpp
git commit -m "feat: add watchdog state machine and failure outcomes"
```

---

### Task 3: Documentation and Final Gate

**Files:**
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-15-cpp-watchdog-state-machine.md` (as-built amendments)

**Interfaces:**
- Consumes: the state-machine API from Task 2.
- Produces: user-facing state-machine documentation and the recorded final gate.

- [ ] **Step 1: Document the state-machine contract in native/README.md**

Append this section to `native/README.md`:

````markdown
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
````

Update the `Layout` section with:

```markdown
- `src/core/state_machine.*` owns the platform-neutral lifecycle and failure policy.
- `src/core/failure_policy.*` owns failure escalation and the COM error window.
```

- [ ] **Step 2: Run the final gate**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
CHROME_BIN=/home/bingmoe/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome npm test
```

Expected:

- Native unit tests: 145/145 pass.
- CTest: 8/8 pass.
- `-Werror` build with no warnings.
- `npm test`: expected 3/3 with the `CHROME_BIN` set above. Without `CHROME_BIN`, the existing contract smoke test fails with `Chrome did not start`.

- [ ] **Step 3: Record as-built amendments**

At the end of this plan file, add an `As-Built Amendments (2026-11-15)` section with the actual unit/CTest counts, any deviations from the code blocks above, and deferred items assigned to Plan 4-8. Record the Task 2 review fixes: LaunchEdge retries kill the owned Edge first; the dead retry branches were removed; CoverDone timeout preserves the target slide. Do not leave placeholders.

- [ ] **Step 4: Commit**

```bash
git add native/README.md \
  docs/superpowers/plans/2026-11-15-cpp-watchdog-state-machine.md
git commit -m "docs: record Plan 3 as-built amendments"
```

---

## Self-Review

### Plan 3 spec coverage

| Spec requirement | Plan 3 coverage |
|---|---|
| State table: Boot, Warming, Monitoring, CoverIn, EdgeStarting, ShowExit, Live, Returning, ReturnCoverOut, Stopping, Faulted, SafeMode, Exited | Task 2 transition engine |
| Events: slideChanged, showEnded, showStarted, coverHold, coverDone, edgeReady, edgeClosed, returnRequested, timeout, stopRequested, powerResume, configReloaded, failure, supervisorLost | Task 1 vocabulary, Task 2 dispatch |
| Guards: ignoreUntil, prevSlide, doneFlags, currentTarget, returnPrepared, edgeDeadline, triggerGeneration, comErrorWindow | Task 2 members and `Snapshot()` |
| Invariants: one Edge launch per generation; done before launch; returnPrepared prevents duplicate resume; cover-ready only after WPS resume; no permanent overlay | Task 2 action ordering |
| Trigger rule: forward leave from target, once per show; arrive mode | Task 2 `OnSlideChanged` |
| Return signal via page title, Alt+F4 fallback, Edge live timeout | Task 2 `OnReturnRequested`, `OnEdgeClosed`, `OnTimeout` |
| Transient retries with backoff and jitter | Task 2 `BeginRetry`/`ExecuteRetry` |
| Switch failure: hide cover, kill Edge; Safe/Legacy policy or consecutive threshold enters SafeMode without `RestoreShow`, otherwise restore WPS and self-heal or keep done per target policy | Task 2 `HandleSwitchFailure` |
| Return failure: hide cover, diagnostic, keep Edge, Faulted, edgeClosed logs only, manual retry | Task 2 `HandleReturnFailure`, `OnEdgeClosed`, `OnReturnRequested` |
| Structural failure: cleanup, SafeMode, legacy fallback | Task 2 `HandleStructuralFailure` |
| Stop/timeout cleanup | Task 2 `BeginCleanup`, `OnCleanupDone`, timer handlers |
| Supervisor loss continue/cleanup | Task 2 `OnSupervisorLost` |
| Monotonic time only | Injected `Clock`, `ClockPoint`, `Duration` |
| Platform neutrality | No Windows headers or COM/Win32 calls |
| Journal key words | `trigger slide N -> Label`, `return requested by page`, `resumed at slide N`, `edge closed after return; cover fade out`, `COM busy`, `cleanup start`, `cleanup done` |

Deferred to Plan 4-8: Plan 4 covers supervisor/worker lifecycle, Job Object, IPC, heartbeat, crash dump, and black-box wiring; Plan 5 covers the WPS COM host plus overlay/focus/Edge execution, compatibility flags, logs, and the stop entry; Plan 6 covers event-driven scheduling, thread priorities, power policy, power-resume timer re-arming, fault injection, and performance instrumentation; Plan 7 covers config hot reload and the configuration UI; Plan 8 covers Windows end-to-end acceptance and release packaging. `DegradedNoSupervisor` is represented by `StateSnapshot.degraded_no_supervisor` while the lifecycle state continues, because the worker is expected to finish the current flow.

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.

### Type consistency

- `Action`, `Event`, `TargetPlan`, `RunPlan`, `StateMachineConfig`, and `StateSnapshot` are defined in Task 1 and consumed unchanged in Task 2.
- `StateMachine::Handle` returns `std::vector<Action>` for every event kind.
- `TimeoutKind` values match the six deadline members.
- `RetryOperation` values match every action emitted by `ExecuteRetry`.
- `EscalateFailure` returns the same `FailureKind` values used by `HandleSwitchFailure`, `HandleReturnFailure`, and `HandleStructuralFailure`.
- `StateMachine::Snapshot` preserves the Plan 1 `State` enum and adds only state-machine guards.

---

## Execution Handoff

Plan 3 is ready. Execute Tasks 1-3 in order. Each task ends with a green full suite and a commit. Task 3 records the final gate and any as-built deviations.

---

## As-Built Amendments (2026-11-15)

Plan 3 was executed end to end on branch `worktree-feat-cpp-watchdog-state-machine`. Commits: `cbbf6a4` (Task 1), `24ca3c3` (Task 2), `2da1819` (plan correction for the Task 2 review), `957e5ce` (Task 2 fix round), and the Task 3 documentation commits. The worktree was clean after each commit.

### Final gate (observed)

Commands, run from this worktree's root:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
CHROME_BIN=/home/bingmoe/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome npm test
```

Observed results:

- Configure: success, exit 0.
- `-Werror` build: exit 0, zero `warning` or `error` lines in the full build log.
- Unit suite: 145 `[PASS]`, 0 `[FAIL]`, `ALL TESTS PASSED`, exit 0. The 145 cases are 97 pre-existing cases + 6 Task 1 failure-policy cases + 34 Task 2 state-machine cases + 1 Task 2 fix-round regression case (`state_machine_launch_retry_kills_edge_first`) + 7 final-review tests.
- CTest: 8/8 passed, 100%, exit 0.
- `npm test` with `CHROME_BIN`: 3/3 pass, 0 fail, exit 0. Without `CHROME_BIN` the existing contract smoke test fails with `Chrome did not start`, so the variable is required in this WSL environment.

### Source as built

- Task 1 created `native/src/core/state_machine_types.h`, `native/src/core/state_machine_types.cpp`, `native/src/core/failure_policy.h`, `native/src/core/failure_policy.cpp`, and `native/tests/unit/failure_policy_test.cpp` (404 insertions, no existing files changed).
- Task 2 created `native/src/core/state_machine.h`, `native/src/core/state_machine.cpp`, and `native/tests/unit/state_machine_test.cpp` (1360 insertions, no existing files changed).
- The Task 2 fix round modified only those three Task 2 files (31 insertions, 16 deletions).
- No CMake changes were needed: `src/core/*.cpp` and `tests/unit/*.cpp` are globbed with `CONFIGURE_DEPENDS`, and the new sources were picked up on the automatic re-configure.
- No existing source, script, root test, or production documentation was modified by this plan.

### Approved deviations and fixes

1. `(void)event;` in `StateMachine::OnEdgeReady`. The planned implementation never reads `event`, and GCC rejects the unused parameter under `-Wall -Wextra -Wpedantic -Werror`. No behavior change; the same pattern is used by `OnCleanupDone`.
2. Designated initialization with explicit `std::nullopt` in the test `MakePlan()` helper. The planned positional aggregate initialization set only three of the eight `TargetPlan` members; `max_consecutive_trigger_failures` and `cover_max_hold` are `std::optional` members without default member initializers, so `-Wextra` raised `-Werror=missing-field-initializers` (GCC also warns for designated initialization when such a member is omitted). The helper now uses designated initializers and sets both members to `std::nullopt`. Slide, label, edge profile, trigger mode, failure policy, and enabled state are unchanged, so every test case keeps its planned semantics.
3. `LaunchEdge` retries emit `KillEdge` before `LaunchEdge` (commit `957e5ce`). A transient failure in `EdgeStarting` now kills the owned Edge job before relaunching, which enforces the one-Edge-process-per-trigger-generation invariant instead of relying on undocumented host behavior. The EdgeReady deadline is still re-armed before the actions. Covered by the added regression test `state_machine_launch_retry_kills_edge_first`, which asserts both actions are present and that `KillEdge` precedes `LaunchEdge`.
4. Dead `RetryOperation` branches removed. `OnFailure` never assigned `ResumeShow`, `FocusEdge`, or `FocusWps`, so those enumerators and their `ExecuteRetry` cases were deleted. `RetryOperation` now contains only `None`, `CoverFadeIn`, `LaunchEdge`, and `ExitShow`; `None` still returns an empty action list.
5. Unused `#include <algorithm>` removed from `state_machine.cpp`.
6. `CoverDone` timeout preserves the target slide. The handler captures `current_target_slide_.value_or(0)` before resetting the target and uses that value for the `Journal`, `CoverHide`, and `Diagnostic` actions, which previously hard-coded `0`. `state_machine_cover_done_timeout_hides_cover_and_monitors` now also asserts `CoverHide.slide == 5`.
7. The Task 3 review corrected the README switch-failure contract: Safe, legacy, and threshold-exceeded paths enter SafeMode without `RestoreShow`. The as-built deferred ownership now follows the Plan 1 roadmap (Plans 4-8).
8. The final review added tests for ReturnResume timeout, Returning retry/exhaustion, ShowEnded during return, EdgeClosed before ready, COM success reset, and both per-target overrides; it also made EscalateFailure explicit, reset the COM window on switch failure, clarified return_prepared_, and documented the host contracts above.

The Task 2 code blocks in this plan were corrected in commit `2da1819` so the plan text and the implemented behavior agree.

### Plan 4/5 host contracts

- `showStarted` must mean a genuinely new slideshow. The `SlideShowBegin` notification produced by `ResumeShow` must not be forwarded as `showStarted`; forwarding it clears `done`, and the same target can trigger again.
- Events carry no trigger generation. The host must tag or filter events so a late `edgeReady`, `edgeClosed`, or `showEnded` from a previous trigger cannot reach the current generation.
- `ShowExit` has no built-in timer. If `ExitShow` does not produce `showEnded`, the host must synthesize a switch failure; otherwise the cover can remain visible.
- Execute action batches in order and abort the remainder when an action fails. A failed `ResumeShow` must not be followed by `CoverFadeOut`.
- `KillEdge` and `RestoreShow` must be idempotent. Because `LaunchEdge` retries are preceded by `KillEdge` (deviation 3), the Windows host can receive a kill request for an Edge job that is already gone.
- COM failures are evaluated when the next COM failure event arrives; the error window is not a timer.
- Cover fade/hold and focus timings are host-facing fields; the host maps the Plan 2 configuration into `StateMachineConfig` and `TargetPlan`.

### Deferred to Plan 4-8

- Plan 4: supervisor/worker lifecycle, Job Object, IPC, heartbeat, crash
  dump, and black-box wiring.
- Plan 5: WPS COM host; overlay, focus, and Edge job execution; compatibility
  flags, logs, and the stop entry.
- Plan 6: event-driven scheduling, thread priorities, power policy,
  power-resume timer re-arming, fault injection, and performance
  instrumentation.
- Plan 7: configuration hot reload (including an `EventKind::ConfigReloaded`
  factory) and the configuration UI.
- Plan 8: Windows end-to-end acceptance and release packaging.

Minor coverage gaps recorded by the Task 1 and Task 2 reviews, left open by design:

- Event factories have no direct smoke tests; they are exercised indirectly by the state-machine cases. `EventKind::ConfigReloaded` has no factory yet and is deferred with hot reload.
- `Event` defaults to `Timeout`/`Retry` by design, so a default-constructed event is not meaningful.
- Design choice: the spec's `showResumed` and `workerFault` events are folded into `CoverHold` and `Failure`; Faulted emits cover hide, journal, and diagnostic actions plus manual retry, rather than an automatic WPS restore.

Accepted behavior: SafeMode is latched and has no exit transition in this plan, so leaving it requires an external stop.
