# C++ Watchdog Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the platform-agnostic foundation of the C++ watchdog: CMake project, deterministic clock, retry/restart policy, JSONL journal, and scheduling policy, with a working test suite.

**Architecture:** A static `htmlppt_core` library with no Win32 dependency, a thin console executable, and a tiny header-only test harness. Later plans layer configuration, state machine, supervisor/IPC, WPS/overlay/focus, scheduling integration, and the configuration UI on top.

**Tech Stack:** C++20, CMake 3.20+, nlohmann/json 3.11.3 (vendored single header), CTest, minimal custom test harness.

## Global Constraints

- Target Windows 10/11 x64; Plan 1 must also build and test on Linux/WSL.
- Prerequisites: CMake 3.20+, a C++20 compiler (GCC 11+, Clang 14+, or MSVC 2022), and curl for the one-time JSON vendor step.
- No third-party runtime dependencies beyond the vendored nlohmann/json single header.
- C++20; MSVC uses `/W4 /permissive- /utf-8`; GCC/Clang use `-Wall -Wextra -Wpedantic`.
- All new code lives under `native/`. Do not modify existing `scripts/`, `tests/`, or production docs except the approved spec and this plan.
- Existing `npm test` must stay green; Plan 1 does not modify the Node test suite.
- Use ASCII-only source files. User-visible strings and logs are UTF-8 at runtime.
- Do not add Windows-specific headers in Plan 1. `core/` must compile on Linux.
- Every task ends with a commit. Do not push.

## Scope Check

The approved spec covers several independently testable subsystems. Delivering them in one plan would create an unreviewable change set, so the implementation is split into eight plans. Each plan produces a working, testable slice and defines interfaces consumed by the next.

| Plan | Scope | Deliverable | Depends on |
|---|---|---|---|
| **1. Foundation (this plan)** | CMake, test harness, clock, retry/restart, journal, scheduling policy | `htmlppt_core` library + tests + `--version` executable | none |
| 2. Config and resolve | Config model, legacy arrays, profiles, validation, deck-info provider, target resolution, `--validate` / `--resolve` / `--print-config` / `--print-schema` | Working config CLI on Linux/Windows | Plan 1 |
| 3. State machine | All states, guards, failure policies, journal integration, simulation tests | Deterministic state machine library | Plans 1-2 |
| 4. Supervisor and worker | Windows Job, IPC, heartbeat, blackbox, crash dump, restart budget, safe mode | Supervisor/worker skeleton with fault tests | Plans 1-3 |
| 5. Platform integration | WPS COM host, event sink, overlay, focus, Edge Job/window detection, compatibility flags, stop shim | Functional watchdog on WPS + Edge | Plans 1-4 |
| 6. Scheduling integration | High-resolution timers, priority tiers, EcoQoS, power policy, fault injection, benchmarks | Aggressive/adaptive scheduling in the real process | Plans 1-5 |
| 7. Configuration UI | Editor model, Win32 view, auto-map wizard, atomic save, hot reload interaction | `--config-ui` native configurator | Plans 1-2 |
| 8. Hardening and release | End-to-end WPS/Edge acceptance, multi-monitor/DPI/sleep, legacy fallback, MSVC/MinGW release, docs, SHA256 | Release candidate | Plans 1-7 |

Plan 1 is intentionally platform-neutral. It contains no COM, window, job, or overlay code.

## Plan 1 File Structure

```text
native/
  CMakeLists.txt
  README.md
  src/
    app/
      main.cpp
    core/
      version.h.in
      version.cpp
      clock.h
      clock.cpp
      retry_policy.h
      retry_policy.cpp
      journal.h
      journal.cpp
      types.h
      scheduling.h
      scheduling.cpp
  tests/
    test_main.h
    test_utils.h
    unit/
      test_main.cpp
      smoke_test.cpp
      json_test.cpp
      clock_test.cpp
      retry_policy_test.cpp
      journal_test.cpp
      scheduling_test.cpp
  third_party/
    nlohmann/json.hpp
    nlohmann/LICENSE.MIT
```

---

## Task 1: Scaffold CMake, test harness, and version

**Files:**
- Create: `native/CMakeLists.txt`
- Create: `native/README.md`
- Create: `native/src/core/version.h.in`
- Create: `native/src/core/version.cpp`
- Create: `native/src/app/main.cpp`
- Create: `native/tests/test_main.h`
- Create: `native/tests/unit/test_main.cpp`
- Create: `native/tests/unit/smoke_test.cpp`
- Modify: `.gitignore` (append native build output)

**Interfaces:**
- Produces: `htmlppt::version() -> std::string_view`, `htmlppt::kVersion`
- Produces: `HTMLPPT_TEST_CASE`, `HTMLPPT_CHECK`, `HTMLPPT_CHECK_EQ`
- Produces: CMake targets `htmlppt_core`, `htmlppt-watchdog`, `htmlppt_core_tests`

- [ ] **Step 1: Write the failing smoke test**

Create the directory skeleton first:

```bash
mkdir -p native/src/core native/src/app native/tests/unit native/third_party/nlohmann
```

Create `native/tests/unit/smoke_test.cpp`:

```cpp
#include "test_main.h"

#include <string_view>

#include "core/version.h"

HTMLPPT_TEST_CASE(version_is_expected) {
  HTMLPPT_CHECK_EQ(htmlppt::version(), std::string_view{"0.1.0"});
}
```

- [ ] **Step 2: Run the build and verify it fails**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON
```

Expected: FAIL because `native/CMakeLists.txt` does not exist.

- [ ] **Step 3: Create the scaffold**

Create `native/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(htmlppt_watchdog VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HTMLPPT_BUILD_TESTS "Build unit tests" ON)
option(HTMLPPT_WERROR "Treat warnings as errors" OFF)

function(htmlppt_enable_warnings target)
  if (MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    if (HTMLPPT_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if (HTMLPPT_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h"
  @ONLY)

file(GLOB HTMLPPT_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
add_library(htmlppt_core STATIC ${HTMLPPT_CORE_SOURCES})
target_include_directories(htmlppt_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_features(htmlppt_core PUBLIC cxx_std_20)
htmlppt_enable_warnings(htmlppt_core)

add_executable(htmlppt-watchdog src/app/main.cpp)
target_link_libraries(htmlppt-watchdog PRIVATE htmlppt_core)
htmlppt_enable_warnings(htmlppt-watchdog)

if (HTMLPPT_BUILD_TESTS)
  enable_testing()
  file(GLOB HTMLPPT_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp")
  add_executable(htmlppt_core_tests ${HTMLPPT_TEST_SOURCES})
  target_include_directories(htmlppt_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(htmlppt_core_tests PRIVATE htmlppt_core)
  htmlppt_enable_warnings(htmlppt_core_tests)
  add_test(NAME core_tests COMMAND htmlppt_core_tests)
  set_tests_properties(core_tests PROPERTIES TIMEOUT 30)

  add_test(NAME cli_version COMMAND htmlppt-watchdog --version)
  set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "htmlppt-watchdog 0[.]1[.]0")
  add_test(NAME cli_help COMMAND htmlppt-watchdog --help)
  set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: htmlppt-watchdog")
endif()
```

Create `native/src/core/version.h.in`:

```cpp
#pragma once

#include <string_view>

namespace htmlppt {

inline constexpr std::string_view kVersion = "@PROJECT_VERSION@";

std::string_view version() noexcept;

} // namespace htmlppt
```

Create `native/src/core/version.cpp`:

```cpp
#include "core/version.h"

namespace htmlppt {

std::string_view version() noexcept {
  return kVersion;
}

} // namespace htmlppt
```

Create `native/src/app/main.cpp`:

```cpp
#include "core/version.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc <= 1) {
    std::cout << "htmlppt-watchdog " << htmlppt::version() << "\n";
    return 0;
  }

  const std::string_view argument = argv[1];
  if (argument == "--version") {
    std::cout << "htmlppt-watchdog " << htmlppt::version() << "\n";
    return 0;
  }
  if (argument == "--help" || argument == "-h") {
    std::cout << "usage: htmlppt-watchdog [--version] [--help]\n";
    return 0;
  }

  std::cerr << "unknown option: " << argument << "\n";
  return 2;
}
```

Create `native/tests/test_main.h`:

```cpp
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace htmlppt::test {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct TestCase {
  std::string name;
  std::function<void()> function;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> function) {
    registry().push_back({std::move(name), std::move(function)});
  }
};

inline int run_all() {
  int failed = 0;
  for (const auto& test : registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << "\n";
    } catch (const Failure& error) {
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << "\n";
      ++failed;
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test.name
                << ": unexpected exception: " << error.what() << "\n";
      ++failed;
    } catch (...) {
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
      ++failed;
    }
  }
  std::cout << (failed == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << "\n";
  return failed == 0 ? 0 : 1;
}

} // namespace htmlppt::test

#define HTMLPPT_TEST_CASE(name)                                      \
  static void name();                                                \
  static const ::htmlppt::test::Registrar name##_registrar(#name, name); \
  static void name()

#define HTMLPPT_CHECK(condition)                                      \
  do {                                                                \
    if (!(condition)) {                                               \
      std::ostringstream message;                                     \
      message << __FILE__ << ":" << __LINE__                          \
              << ": CHECK(" #condition ") failed";                    \
      throw ::htmlppt::test::Failure(message.str());                  \
    }                                                                 \
  } while (0)

#define HTMLPPT_CHECK_EQ(lhs, rhs)                                    \
  do {                                                                \
    const auto& left_value = (lhs);                                   \
    const auto& right_value = (rhs);                                  \
    if (!(left_value == right_value)) {                               \
      std::ostringstream message;                                     \
      message << __FILE__ << ":" << __LINE__                          \
              << ": CHECK_EQ(" #lhs ", " #rhs ") failed";             \
      throw ::htmlppt::test::Failure(message.str());                  \
    }                                                                 \
  } while (0)
```

Create `native/tests/unit/test_main.cpp`:

```cpp
#include "test_main.h"

int main() {
  return htmlppt::test::run_all();
}
```

Create `native/README.md`:

````markdown
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
- `src/app/` contains the executable entry point.
- `tests/unit/` contains test cases registered with `HTMLPPT_TEST_CASE`.
- `third_party/` contains vendored single-header dependencies.
````

Append to `.gitignore`:

```text

# Native build output
native/build/
```

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected:

```text
[PASS] version_is_expected
ALL TESTS PASSED
100% tests passed
```

- [ ] **Step 5: Commit**

```bash
git add .gitignore native/CMakeLists.txt native/README.md native/src native/tests
git commit -m "build: scaffold native watchdog core and test harness"
```

---

## Task 2: Vendor nlohmann/json and add a parse smoke test

**Files:**
- Create: `native/third_party/nlohmann/json.hpp` (downloaded)
- Create: `native/third_party/nlohmann/LICENSE.MIT` (downloaded)
- Create: `native/tests/unit/json_test.cpp`

**Interfaces:**
- Produces: `#include <nlohmann/json.hpp>` available to all core code and tests.

- [ ] **Step 1: Write the failing JSON test**

Create `native/tests/unit/json_test.cpp`:

```cpp
#include "test_main.h"

#include <string>

#include <nlohmann/json.hpp>

HTMLPPT_TEST_CASE(json_parses_object) {
  const auto value = nlohmann::json::parse(R"({"a":1,"b":"x"})");
  HTMLPPT_CHECK_EQ(value["a"].get<int>(), 1);
  HTMLPPT_CHECK_EQ(value["b"].get<std::string>(), std::string{"x"});
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `nlohmann/json.hpp` cannot be found.

- [ ] **Step 3: Vendor the pinned single header**

Run:

```bash
mkdir -p native/third_party/nlohmann
curl -fsSL -o native/third_party/nlohmann/json.hpp \
  https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
curl -fsSL -o native/third_party/nlohmann/LICENSE.MIT \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT
grep -E 'NLOHMANN_JSON_VERSION_(MAJOR|MINOR|PATCH)' \
  native/third_party/nlohmann/json.hpp
```

Expected:

```text
#define NLOHMANN_JSON_VERSION_MAJOR 3
#define NLOHMANN_JSON_VERSION_MINOR 11
#define NLOHMANN_JSON_VERSION_PATCH 3
```

If the development machine is offline, copy the same version from a local package cache or mirror; the version macros must still print 3.11.3.

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: all tests pass, including `json_parses_object`.

- [ ] **Step 5: Commit**

```bash
git add native/third_party/nlohmann native/tests/unit/json_test.cpp
git commit -m "build: vendor nlohmann json 3.11.3"
```

---

## Task 3: Deterministic clock abstraction

**Files:**
- Create: `native/src/core/clock.h`
- Create: `native/src/core/clock.cpp`
- Create: `native/tests/unit/clock_test.cpp`

**Interfaces:**
- Produces: `htmlppt::ClockPoint`, `htmlppt::Duration`
- Produces: `htmlppt::Clock` with `now()` and `sleep_until()`
- Produces: `htmlppt::SteadyClock`, `htmlppt::FakeClock`
- Produces: `FakeClock::advance(Duration)`, `FakeClock::sleeps()`

- [ ] **Step 1: Write the failing clock tests**

Create `native/tests/unit/clock_test.cpp`:

```cpp
#include "test_main.h"

#include <chrono>
#include <cstddef>

#include "core/clock.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::SteadyClock;

HTMLPPT_TEST_CASE(fake_clock_advances) {
  FakeClock clock;
  const auto start = clock.now();
  clock.advance(Duration{250});
  HTMLPPT_CHECK_EQ(
      std::chrono::duration_cast<Duration>(clock.now() - start).count(), 250);
}

HTMLPPT_TEST_CASE(fake_clock_records_sleeps) {
  FakeClock clock;
  const auto start = clock.now();
  clock.sleep_until(start + Duration{100});
  HTMLPPT_CHECK_EQ(clock.sleeps().size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(
      std::chrono::duration_cast<Duration>(clock.now() - start).count(), 100);
}

HTMLPPT_TEST_CASE(steady_clock_is_non_decreasing) {
  SteadyClock clock;
  const auto first = clock.now();
  const auto second = clock.now();
  HTMLPPT_CHECK(second >= first);
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/clock.h` does not exist.

- [ ] **Step 3: Implement the clock**

Create `native/src/core/clock.h`:

```cpp
#pragma once

#include <chrono>
#include <thread>
#include <vector>

namespace htmlppt {

using ClockPoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::milliseconds;

class Clock {
 public:
  virtual ~Clock() = default;
  virtual ClockPoint now() const = 0;
  virtual void sleep_until(ClockPoint deadline) = 0;
};

class SteadyClock final : public Clock {
 public:
  ClockPoint now() const override {
    return std::chrono::steady_clock::now();
  }

  void sleep_until(ClockPoint deadline) override {
    if (deadline > now()) {
      std::this_thread::sleep_until(deadline);
    }
  }
};

class FakeClock final : public Clock {
 public:
  explicit FakeClock(ClockPoint start = ClockPoint(Duration{0}))
      : now_(start) {}

  ClockPoint now() const override {
    return now_;
  }

  void sleep_until(ClockPoint deadline) override {
    if (deadline > now_) {
      now_ = deadline;
      sleeps_.push_back(deadline);
    }
  }

  void advance(Duration delta) {
    if (delta.count() > 0) {
      now_ += delta;
    }
  }

  const std::vector<ClockPoint>& sleeps() const {
    return sleeps_;
  }

 private:
  ClockPoint now_;
  std::vector<ClockPoint> sleeps_;
};

} // namespace htmlppt
```

Create `native/src/core/clock.cpp`:

```cpp
#include "core/clock.h"
```

The implementation is header-only; `clock.cpp` exists so the CMake glob has a translation unit and future platform-specific clock code has a home.

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: all tests pass, including the three clock tests.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/clock.h native/src/core/clock.cpp native/tests/unit/clock_test.cpp
git commit -m "feat: add deterministic clock abstraction"
```

---

## Task 4: Retry policy and restart budget

**Files:**
- Create: `native/src/core/retry_policy.h`
- Create: `native/src/core/retry_policy.cpp`
- Create: `native/tests/unit/retry_policy_test.cpp`

**Interfaces:**
- Produces: `htmlppt::RetryConfig`
- Produces: `htmlppt::RetryPolicy::next_delay(int attempt_index) -> std::optional<Duration>`
- Produces: `htmlppt::RetryPolicy::reset()`
- Produces: `htmlppt::RestartBudget::allow(ClockPoint)`, `record(ClockPoint)`

- [ ] **Step 1: Write the failing retry tests**

Create `native/tests/unit/retry_policy_test.cpp`:

```cpp
#include "test_main.h"

#include "core/clock.h"
#include "core/retry_policy.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::RestartBudget;
using htmlppt::RetryConfig;
using htmlppt::RetryPolicy;

HTMLPPT_TEST_CASE(retry_backoff_is_capped) {
  RetryConfig config;
  config.max_attempts = 3;
  config.base_delay = Duration{100};
  config.max_delay = Duration{300};
  config.jitter_ratio = 0.0;

  RetryPolicy policy(config);
  const auto first = policy.next_delay(0);
  const auto second = policy.next_delay(1);
  const auto third = policy.next_delay(2);

  HTMLPPT_CHECK(first.has_value());
  HTMLPPT_CHECK(second.has_value());
  HTMLPPT_CHECK(third.has_value());
  HTMLPPT_CHECK_EQ(first->count(), 100);
  HTMLPPT_CHECK_EQ(second->count(), 200);
  HTMLPPT_CHECK_EQ(third->count(), 300);
  HTMLPPT_CHECK(!policy.next_delay(3).has_value());
}

HTMLPPT_TEST_CASE(retry_jitter_is_bounded_and_deterministic) {
  RetryConfig config;
  config.max_attempts = 3;
  config.base_delay = Duration{100};
  config.max_delay = Duration{1000};
  config.jitter_ratio = 0.5;
  config.seed = 42;

  RetryPolicy first_policy(config);
  RetryPolicy second_policy(config);

  const auto first = first_policy.next_delay(0);
  const auto second = second_policy.next_delay(0);
  HTMLPPT_CHECK(first.has_value());
  HTMLPPT_CHECK(second.has_value());
  HTMLPPT_CHECK(first->count() >= 50);
  HTMLPPT_CHECK(first->count() <= 150);
  HTMLPPT_CHECK_EQ(first->count(), second->count());

  first_policy.reset();
  const auto after_reset = first_policy.next_delay(0);
  HTMLPPT_CHECK(after_reset.has_value());
  HTMLPPT_CHECK_EQ(after_reset->count(), first->count());
}

HTMLPPT_TEST_CASE(restart_budget_expires_old_entries) {
  FakeClock clock;
  RestartBudget budget(2, Duration{10000});

  HTMLPPT_CHECK(budget.allow(clock.now()));
  budget.record(clock.now());
  HTMLPPT_CHECK(budget.allow(clock.now()));
  budget.record(clock.now());
  HTMLPPT_CHECK(!budget.allow(clock.now()));

  clock.advance(Duration{10001});
  HTMLPPT_CHECK(budget.allow(clock.now()));
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/retry_policy.h` does not exist.

- [ ] **Step 3: Implement the retry policy**

Create `native/src/core/retry_policy.h`:

```cpp
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "core/clock.h"

namespace htmlppt {

struct RetryConfig {
  int max_attempts = 3;
  Duration base_delay{200};
  Duration max_delay{5000};
  double jitter_ratio = 0.2;
  std::uint32_t seed = 0x9e3779b9u;
};

class RetryPolicy {
 public:
  explicit RetryPolicy(RetryConfig config);

  std::optional<Duration> next_delay(int attempt_index);
  void reset();

 private:
  std::uint32_t next_random();

  RetryConfig config_;
  std::uint32_t state_;
};

class RestartBudget {
 public:
  RestartBudget(int max_restarts, Duration window);

  bool allow(ClockPoint now);
  void record(ClockPoint now);

 private:
  int max_restarts_;
  Duration window_;
  std::deque<ClockPoint> restarts_;
};

} // namespace htmlppt
```

Create `native/src/core/retry_policy.cpp`:

```cpp
#include "core/retry_policy.h"

#include <algorithm>

namespace htmlppt {

RetryPolicy::RetryPolicy(RetryConfig config)
    : config_(config),
      state_(config.seed == 0 ? 0x9e3779b9u : config.seed) {}

std::uint32_t RetryPolicy::next_random() {
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  return state_;
}

std::optional<Duration> RetryPolicy::next_delay(int attempt_index) {
  if (attempt_index < 0 || attempt_index >= config_.max_attempts) {
    return std::nullopt;
  }

  const int shift = std::min(attempt_index, 20);
  Duration delay = config_.base_delay * (1LL << shift);
  if (delay > config_.max_delay) {
    delay = config_.max_delay;
  }

  if (config_.jitter_ratio > 0.0) {
    const auto max_jitter =
        static_cast<long long>(delay.count() * config_.jitter_ratio);
    if (max_jitter > 0) {
      const auto span = static_cast<std::uint64_t>(2 * max_jitter + 1);
      const auto offset =
          static_cast<long long>(next_random() % span) - max_jitter;
      delay += Duration{offset};
      if (delay < Duration{0}) {
        delay = Duration{0};
      }
    }
  }

  return delay;
}

void RetryPolicy::reset() {
  state_ = config_.seed == 0 ? 0x9e3779b9u : config_.seed;
}

RestartBudget::RestartBudget(int max_restarts, Duration window)
    : max_restarts_(max_restarts), window_(window) {}

bool RestartBudget::allow(ClockPoint now) {
  while (!restarts_.empty() && now - restarts_.front() > window_) {
    restarts_.pop_front();
  }
  return static_cast<int>(restarts_.size()) < max_restarts_;
}

void RestartBudget::record(ClockPoint now) {
  restarts_.push_back(now);
}

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: all tests pass, including the three retry tests.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/retry_policy.h native/src/core/retry_policy.cpp native/tests/unit/retry_policy_test.cpp
git commit -m "feat: add retry policy and restart budget"
```

---

## Task 5: JSONL journal and in-memory journal

**Files:**
- Create: `native/src/core/journal.h`
- Create: `native/src/core/journal.cpp`
- Create: `native/tests/test_utils.h`
- Create: `native/tests/unit/journal_test.cpp`

**Interfaces:**
- Produces: `htmlppt::JournalRecord`
- Produces: `htmlppt::Journal` with `append`, `flush`, `read_tail`
- Produces: `htmlppt::InMemoryJournal`, `htmlppt::JsonlJournal`
- Produces: `htmlppt::test::TempDir`

- [ ] **Step 1: Write the failing journal tests**

Create `native/tests/test_utils.h`:

```cpp
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace htmlppt::test {

class TempDir {
 public:
  TempDir() {
    static std::atomic<unsigned long long> counter{0};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("htmlppt-test-" + std::to_string(stamp) + "-" +
             std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

} // namespace htmlppt::test
```

Create `native/tests/unit/journal_test.cpp`:

```cpp
#include "test_main.h"
#include "test_utils.h"

#include <cstdint>
#include <fstream>
#include <string>

#include "core/journal.h"

using htmlppt::InMemoryJournal;
using htmlppt::JournalRecord;
using htmlppt::JsonlJournal;
using htmlppt::test::TempDir;

HTMLPPT_TEST_CASE(in_memory_journal_assigns_sequences) {
  InMemoryJournal journal;
  JournalRecord record;
  record.run_id = "run-1";
  record.event = "warm";
  record.state = "Warming";
  journal.append(record);

  record.event = "monitor";
  record.state = "Monitoring";
  journal.append(record);

  const auto tail = journal.read_tail(10);
  HTMLPPT_CHECK_EQ(tail.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(tail[0].sequence, std::uint64_t{0});
  HTMLPPT_CHECK_EQ(tail[1].sequence, std::uint64_t{1});
  HTMLPPT_CHECK_EQ(tail[1].event, std::string{"monitor"});
}

HTMLPPT_TEST_CASE(jsonl_journal_round_trips) {
  TempDir directory;
  const auto path = directory.path() / "journal.jsonl";
  {
    JsonlJournal journal(path);
    JournalRecord record;
    record.run_id = "run-2";
    record.event = "cover";
    record.state = "CoverIn";
    journal.append(record);
    record.event = "done";
    journal.append(record);
    journal.flush();
  }

  JsonlJournal reader(path);
  const auto tail = reader.read_tail(1);
  HTMLPPT_CHECK_EQ(tail.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(tail[0].event, std::string{"done"});
  HTMLPPT_CHECK_EQ(tail[0].run_id, std::string{"run-2"});
}

HTMLPPT_TEST_CASE(jsonl_journal_skips_malformed_lines) {
  TempDir directory;
  const auto path = directory.path() / "journal.jsonl";
  {
    std::ofstream output(path);
    output << "not-json\n";
    output << R"({"run_id":"run-3","sequence":1,"timestamp":"t","event":"ok","state":"Live","slide":0,"detail":""})"
           << "\n";
  }

  JsonlJournal journal(path);
  const auto tail = journal.read_tail(10);
  HTMLPPT_CHECK_EQ(tail.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(tail[0].event, std::string{"ok"});
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/journal.h` does not exist.

- [ ] **Step 3: Implement the journal**

Create `native/src/core/journal.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace htmlppt {

struct JournalRecord {
  std::string run_id;
  std::uint64_t sequence = 0;
  std::string timestamp;
  std::string event;
  std::string state;
  int slide = 0;
  std::string detail;
};

class Journal {
 public:
  virtual ~Journal() = default;
  virtual void append(const JournalRecord& record) = 0;
  virtual void flush() = 0;
  virtual std::vector<JournalRecord> read_tail(std::size_t max_records) const = 0;
};

class InMemoryJournal final : public Journal {
 public:
  void append(const JournalRecord& record) override;
  void flush() override;
  std::vector<JournalRecord> read_tail(std::size_t max_records) const override;

 private:
  std::vector<JournalRecord> records_;
  std::uint64_t next_sequence_ = 0;
};

class JsonlJournal final : public Journal {
 public:
  explicit JsonlJournal(std::filesystem::path path);

  void append(const JournalRecord& record) override;
  void flush() override;
  std::vector<JournalRecord> read_tail(std::size_t max_records) const override;

 private:
  std::filesystem::path path_;
  std::ofstream output_;
  std::uint64_t next_sequence_ = 0;
};

} // namespace htmlppt
```

Create `native/src/core/journal.cpp`:

```cpp
#include "core/journal.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace htmlppt {
namespace {

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &time);
#else
  gmtime_r(&time, &parts);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

} // namespace

void InMemoryJournal::append(const JournalRecord& record) {
  auto copy = record;
  copy.sequence = next_sequence_++;
  if (copy.timestamp.empty()) {
    copy.timestamp = utc_timestamp();
  }
  records_.push_back(std::move(copy));
}

void InMemoryJournal::flush() {}

std::vector<JournalRecord> InMemoryJournal::read_tail(
    std::size_t max_records) const {
  if (max_records >= records_.size()) {
    return records_;
  }
  return std::vector<JournalRecord>(records_.end() -
                                        static_cast<std::ptrdiff_t>(max_records),
                                    records_.end());
}

JsonlJournal::JsonlJournal(std::filesystem::path path)
    : path_(std::move(path)) {
  if (!path_.parent_path().empty()) {
    std::filesystem::create_directories(path_.parent_path());
  }
  output_.open(path_, std::ios::app);
}

void JsonlJournal::append(const JournalRecord& record) {
  auto copy = record;
  copy.sequence = next_sequence_++;
  if (copy.timestamp.empty()) {
    copy.timestamp = utc_timestamp();
  }

  nlohmann::json line{
      {"run_id", copy.run_id},
      {"sequence", copy.sequence},
      {"timestamp", copy.timestamp},
      {"event", copy.event},
      {"state", copy.state},
      {"slide", copy.slide},
      {"detail", copy.detail},
  };
  output_ << line.dump() << '\n';
}

void JsonlJournal::flush() {
  output_.flush();
}

std::vector<JournalRecord> JsonlJournal::read_tail(
    std::size_t max_records) const {
  std::ifstream input(path_);
  std::vector<JournalRecord> records;
  std::string text;
  while (std::getline(input, text)) {
    const auto line = nlohmann::json::parse(text, nullptr, false);
    if (line.is_discarded() || !line.is_object()) {
      continue;
    }
    JournalRecord record;
    record.run_id = line.value("run_id", std::string{});
    record.sequence = line.value("sequence", std::uint64_t{0});
    record.timestamp = line.value("timestamp", std::string{});
    record.event = line.value("event", std::string{});
    record.state = line.value("state", std::string{});
    record.slide = line.value("slide", 0);
    record.detail = line.value("detail", std::string{});
    records.push_back(std::move(record));
  }

  if (max_records < records.size()) {
    records.erase(records.begin(),
                  records.end() - static_cast<std::ptrdiff_t>(max_records));
  }
  return records;
}

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: all tests pass, including the three journal tests.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/journal.h native/src/core/journal.cpp native/tests/test_utils.h native/tests/unit/journal_test.cpp
git commit -m "feat: add journal with in-memory and JSONL backends"
```

---

## Task 6: Scheduling policy and shared state types

**Files:**
- Create: `native/src/core/types.h`
- Create: `native/src/core/scheduling.h`
- Create: `native/src/core/scheduling.cpp`
- Create: `native/tests/unit/scheduling_test.cpp`

**Interfaces:**
- Produces: `htmlppt::State`, `SchedulingProfile`, `PowerSource`, `ThreadTier`, `TriggerMode`, `OnFailurePolicy`, `FailureKind`
- Produces: `poll_interval(State, int distance_to_nearest_target, SchedulingProfile, PowerSource) -> Duration`
- Produces: `thread_tier(State) -> ThreadTier`
- Produces: `effective_profile(SchedulingProfile, PowerSource) -> SchedulingProfile`

- [ ] **Step 1: Write the failing scheduling tests**

Create `native/tests/unit/scheduling_test.cpp`:

```cpp
#include "test_main.h"

#include "core/scheduling.h"

using htmlppt::Duration;
using htmlppt::PowerSource;
using htmlppt::SchedulingProfile;
using htmlppt::State;
using htmlppt::ThreadTier;

HTMLPPT_TEST_CASE(adaptive_intervals_follow_state_and_distance) {
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      10);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 2, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::CoverIn, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Live, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      1);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Faulted, 5, SchedulingProfile::Adaptive,
                    PowerSource::AC)
          .count(),
      10);
}

HTMLPPT_TEST_CASE(safe_profile_is_conservative) {
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 5, SchedulingProfile::Safe,
                    PowerSource::AC)
          .count(),
      20);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::CoverIn, 5, SchedulingProfile::Safe,
                    PowerSource::AC)
          .count(),
      5);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Live, 5, SchedulingProfile::Safe, PowerSource::AC)
          .count(),
      10);
}

HTMLPPT_TEST_CASE(battery_downgrades_aggressive_and_adaptive) {
  HTMLPPT_CHECK(
      effective_profile(SchedulingProfile::Aggressive, PowerSource::Battery) ==
      SchedulingProfile::Balanced);
  HTMLPPT_CHECK(
      effective_profile(SchedulingProfile::Adaptive, PowerSource::Battery) ==
      SchedulingProfile::Balanced);
  HTMLPPT_CHECK(
      effective_profile(SchedulingProfile::Safe, PowerSource::Battery) ==
      SchedulingProfile::Safe);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::Monitoring, 5, SchedulingProfile::Aggressive,
                    PowerSource::Battery)
          .count(),
      10);
  HTMLPPT_CHECK_EQ(
      poll_interval(State::CoverIn, 5, SchedulingProfile::Aggressive,
                    PowerSource::Battery)
          .count(),
      2);
}

HTMLPPT_TEST_CASE(thread_tiers_follow_criticality) {
  HTMLPPT_CHECK(thread_tier(State::CoverIn) == ThreadTier::Critical);
  HTMLPPT_CHECK(thread_tier(State::Monitoring) == ThreadTier::Focus);
  HTMLPPT_CHECK(thread_tier(State::Warming) == ThreadTier::Ui);
  HTMLPPT_CHECK(thread_tier(State::Faulted) == ThreadTier::Background);
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/scheduling.h` and `core/types.h` do not exist.

- [ ] **Step 3: Implement the types and scheduling policy**

Create `native/src/core/types.h`:

```cpp
#pragma once

namespace htmlppt {

enum class State {
  Boot,
  Warming,
  Monitoring,
  CoverIn,
  EdgeStarting,
  ShowExit,
  Live,
  Returning,
  ReturnCoverOut,
  Stopping,
  Faulted,
  SafeMode,
  Exited,
  DegradedNoSupervisor,
};

enum class SchedulingProfile {
  Adaptive,
  Aggressive,
  Balanced,
  Safe,
};

enum class PowerSource {
  AC,
  Battery,
};

enum class ThreadTier {
  Supervisor,
  Background,
  Ui,
  Focus,
  Critical,
};

enum class TriggerMode {
  LeaveForward,
  Arrive,
};

enum class OnFailurePolicy {
  SelfHeal,
  Strict,
  Safe,
  Legacy,
};

enum class FailureKind {
  Transient,
  Switch,
  Return,
  Structural,
};

} // namespace htmlppt
```

Create `native/src/core/scheduling.h`:

```cpp
#pragma once

#include "core/clock.h"
#include "core/types.h"

namespace htmlppt {

Duration poll_interval(State state, int distance_to_nearest_target,
                       SchedulingProfile profile, PowerSource power);

ThreadTier thread_tier(State state);

SchedulingProfile effective_profile(SchedulingProfile requested,
                                    PowerSource power);

} // namespace htmlppt
```

Create `native/src/core/scheduling.cpp`:

```cpp
#include "core/scheduling.h"

namespace htmlppt {
namespace {

struct Intervals {
  Duration critical;
  Duration idle;
  Duration live;
  Duration stalled;
};

Intervals intervals_for(SchedulingProfile profile) {
  switch (profile) {
    case SchedulingProfile::Aggressive:
      return {Duration{1}, Duration{5}, Duration{1}, Duration{10}};
    case SchedulingProfile::Adaptive:
      return {Duration{1}, Duration{10}, Duration{1}, Duration{10}};
    case SchedulingProfile::Balanced:
      return {Duration{2}, Duration{10}, Duration{2}, Duration{20}};
    case SchedulingProfile::Safe:
      return {Duration{5}, Duration{20}, Duration{10}, Duration{50}};
  }
  return {Duration{10}, Duration{10}, Duration{10}, Duration{20}};
}

} // namespace

Duration poll_interval(State state, int distance_to_nearest_target,
                       SchedulingProfile profile, PowerSource power) {
  const Intervals intervals = intervals_for(effective_profile(profile, power));

  switch (state) {
    case State::Monitoring:
      if (distance_to_nearest_target >= 0 &&
          distance_to_nearest_target <= 2) {
        return intervals.critical;
      }
      return intervals.idle;
    case State::CoverIn:
    case State::EdgeStarting:
    case State::ShowExit:
    case State::Returning:
    case State::ReturnCoverOut:
      return intervals.critical;
    case State::Live:
      return intervals.live;
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
      return intervals.stalled;
    case State::Boot:
    case State::Warming:
    case State::DegradedNoSupervisor:
      return intervals.idle;
  }

  return intervals.idle;
}

ThreadTier thread_tier(State state) {
  switch (state) {
    case State::CoverIn:
    case State::EdgeStarting:
    case State::ShowExit:
    case State::Live:
    case State::Returning:
    case State::ReturnCoverOut:
      return ThreadTier::Critical;
    case State::Monitoring:
      return ThreadTier::Focus;
    case State::Warming:
    case State::Boot:
    case State::DegradedNoSupervisor:
      return ThreadTier::Ui;
    case State::Stopping:
    case State::Faulted:
    case State::SafeMode:
    case State::Exited:
      return ThreadTier::Background;
  }
  return ThreadTier::Background;
}

SchedulingProfile effective_profile(SchedulingProfile requested,
                                    PowerSource power) {
  if (power == PowerSource::Battery) {
    if (requested == SchedulingProfile::Aggressive ||
        requested == SchedulingProfile::Adaptive) {
      return SchedulingProfile::Balanced;
    }
  }
  return requested;
}

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: all tests pass, including the four scheduling tests.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/types.h native/src/core/scheduling.h native/src/core/scheduling.cpp native/tests/unit/scheduling_test.cpp
git commit -m "feat: add scheduling policy and shared state types"
```

---

## Task 7: Final CLI gate and full verification

**Files:**
- Modify: `native/src/app/main.cpp` (only if the existing help/version behavior needs adjustment)
- Modify: `native/CMakeLists.txt` (only if CLI tests need adjustment)

**Interfaces:**
- Consumes: `htmlppt::version()`
- Produces: stable `--version` and `--help` behavior covered by CTest

- [ ] **Step 1: Confirm the CLI behavior with a test-first check**

Run:

```bash
ctest --test-dir native/build -R "cli_version|cli_help" --output-on-failure
```

Expected: PASS for both tests. If either fails, adjust `native/src/app/main.cpp` so that:

```cpp
if (argument == "--version") {
  std::cout << "htmlppt-watchdog " << htmlppt::version() << "\n";
  return 0;
}
if (argument == "--help" || argument == "-h") {
  std::cout << "usage: htmlppt-watchdog [--version] [--help]\n";
  return 0;
}
```

- [ ] **Step 2: Run the full native test suite with warnings as errors**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected:

```text
[PASS] version_is_expected
[PASS] json_parses_object
[PASS] fake_clock_advances
[PASS] fake_clock_records_sleeps
[PASS] steady_clock_is_non_decreasing
[PASS] retry_backoff_is_capped
[PASS] retry_jitter_is_bounded_and_deterministic
[PASS] restart_budget_expires_old_entries
[PASS] in_memory_journal_assigns_sequences
[PASS] jsonl_journal_round_trips
[PASS] jsonl_journal_skips_malformed_lines
[PASS] adaptive_intervals_follow_state_and_distance
[PASS] safe_profile_is_conservative
[PASS] battery_downgrades_aggressive_and_adaptive
[PASS] thread_tiers_follow_criticality
ALL TESTS PASSED
100% tests passed, 0 tests failed out of 3
```

The order and exact test count may vary; all tests must pass, and CTest must report 0 failures.

- [ ] **Step 3: Run the existing project baseline**

Run:

```bash
npm test
```

Expected: the existing 3 Node tests pass. Plan 1 must not break them.

- [ ] **Step 4: Commit the final verification state**

If Step 1 required a source change:

```bash
git add native/src/app/main.cpp native/CMakeLists.txt
git commit -m "test: lock down native CLI version and help output"
```

If no change was required, there is nothing to commit for this task; record the verification output in the task report.

---

## Self-Review

### Spec coverage for Plan 1

| Spec area | Plan 1 coverage | Later plan |
|---|---|---|
| CMake and native layout | Task 1 | Plan 8 release matrix |
| Monotonic clock / timing foundation | Task 3 | Plans 3, 6 |
| Retry/backoff/restart budget | Task 4 | Plans 3, 4 |
| Journal / black-box persistence foundation | Task 5 | Plans 3, 4 |
| Scheduling profile and poll interval policy | Task 6 | Plan 6 integration |
| Test harness and CI-ready CTest wiring | Tasks 1-7 | Plan 8 |
| Config model, legacy arrays, resolve | not in Plan 1 | Plan 2 |
| State machine and failure policies | not in Plan 1 | Plan 3 |
| Supervisor/worker/IPC/heartbeat/dump | not in Plan 1 | Plan 4 |
| WPS COM/overlay/focus/Edge/compat flags | not in Plan 1 | Plan 5 |
| Aggressive timers/priorities/power/faults | policy in Task 6 | Plan 6 |
| Configuration UI | not in Plan 1 | Plan 7 |
| End-to-end acceptance and release | not in Plan 1 | Plan 8 |

No spec requirement is orphaned; each is assigned to a plan in the roadmap.

### Placeholder scan

- No `TBD`, `TODO`, `implement later`, or empty steps.
- Every code step contains the code to write.
- Every run step contains the exact command and expected result.
- Commands that depend on a network fetch use a pinned version and a verification command.

### Type consistency

- `ClockPoint` and `Duration` are defined in `core/clock.h` and used consistently.
- `State`, `SchedulingProfile`, `PowerSource`, `ThreadTier`, `TriggerMode`, `OnFailurePolicy`, and `FailureKind` are defined once in `core/types.h`.
- `RetryPolicy::next_delay(int)` returns `std::optional<Duration>`.
- `Journal::read_tail(std::size_t)` returns `std::vector<JournalRecord>`.
- `poll_interval(State, int, SchedulingProfile, PowerSource)` and `thread_tier(State)` match later plan interfaces.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-11-14-cpp-watchdog-foundation.md`. Two execution options:

1. **Subagent-Driven (recommended)** - dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** - execute tasks in this session using `superpowers:executing-plans`, batch execution with checkpoints.

Which approach?

---

## As-Built Amendments (2026-11-14)

Plan 1 was executed on branch `worktree-feat-cpp-watchdog-foundation`. The following approved amendments differ from the code blocks above and are the source of truth for the implemented foundation.

1. **Task 4 retry policy**: `max_delay` is a hard upper bound. Jitter is applied before the final clamp, and the jitter calculation uses an explicit `static_cast<double>` to avoid MSVC C4244. Tests assert that jitter is active and that the result never exceeds `max_delay`.

2. **Task 5 journal**: the `Journal` interface exposes `healthy()` and `last_error()`. `JsonlJournal` throws on construction open failure (human-approved deviation), marks itself unhealthy on serialization, write, flush, and read-open failures, and `append` catches invalid UTF-8 instead of letting `nlohmann::json` exceptions escape. Tests cover invalid UTF-8, write failure on Linux, and read-open failure on Linux.

3. **Task 6 scheduling**: Monitoring has distinct far / near / at-target tiers matching spec 7.2. `Intervals` uses C++20 designated initializers. `thread_tier` is role-based via the new `ThreadRole` enum and matches spec 7.3; the old `thread_tier(State)` API was removed.

4. **Task 7 final gate**: CTest 3/3, unit binary 21/21, `npm test` 3/3, `-Werror` clean, working tree clean.

5. **Task 7 unit count**: the plan's expected 17 unit cases became 21 after the final-review fixes (three journal failure-channel tests and one retry hard-bound test).

Deferred to later plans: `JournalRecord` schema expansion, journal sequence recovery, tail-reading optimization, `ThreadTier::Supervisor` to `BELOW_NORMAL` mapping in Plan 6, Windows MSVC/MinGW CI verification in Plan 8, and vendor provenance README / `.gitattributes` hardening.
