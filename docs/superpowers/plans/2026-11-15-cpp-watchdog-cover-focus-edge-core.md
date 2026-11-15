# C++ Watchdog Cover, Focus, Edge, and Compatibility Core (Plan 5B-1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the platform-neutral core needed by the Windows worker host: cover fade/hold timeline, window selection and focus control, Edge launch specification and lifecycle controller, and the compatibility flag/log layer.

**Architecture:** These units stay in `core/` and depend only on `Clock`, `ResolvedTarget`, and standard C++/nlohmann. They expose small interfaces that the next plan (Plan 5B-2) maps onto Win32 layered windows, `EnumWindows`, foreground unlocking, and `CreateProcessW`/Job Objects. Every unit is testable on Linux with fakes, so the Windows mapping can be added without redesigning behavior.

**Tech Stack:** C++20, CMake/CTest, nlohmann/json 3.11.3, the existing `Clock`, `ResolvedTarget`, and test harness.

## Global Constraints

- Target Windows 10/11 x64; Plan 5B-1 must build and test on Linux/WSL.
- C++20; MSVC `/W4 /permissive- /utf-8 /WX`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only.
- `core/` must not include Windows headers. Platform APIs are Plan 5B-2.
- Existing Linux gate: 191/191 unit cases, CTest 8/8, `npm test` 3/3.
- Every task ends with a commit; never push.

## Scope

Plan 5B-1 implements:

- `CoverTimeline`: fade-in, minimum hold, maximum hold safety, fade-out, done edge.
- `CoverOverlay` interface and `CoverController`: drives opacity/show/hide/topmost.
- `WindowSystem` interface, `WindowInfo`, focus ranking for WPS show windows and Edge windows, and `FocusController` polling.
- `EdgeLaunchSpec` URL/argument construction and `EdgeController` lifecycle over an `EdgeLauncher` interface.
- `CompatFlags` and `LegacyLog`: run-id ownership, lock JSON line, legacy flag mirror, log append.
- Unit tests for every unit with fake window systems, fake overlays, fake Edge launchers, and temporary directories.

Not in Plan 5B-1: Win32 layered window creation, `EnumWindows`/foreground unlocking, `CreateProcessW`, Job Objects, WinEvent hooks, worker orchestration, and the legacy `stop-live.vbs` shim. These are Plan 5B-2/5C.

## File Structure

```text
native/
  src/core/
    cover_overlay.h          # CoverOverlay interface
    cover_timeline.h/.cpp    # fade/hold/fade-out timeline
    cover_controller.h/.cpp  # timeline + overlay driver
    window_system.h          # WindowId/WindowInfo/WindowSystem interface
    focus_model.h/.cpp       # WPS/Edge window selection and return-title check
    focus_controller.h/.cpp  # polling focus loops over WindowSystem + Clock
    edge_spec.h/.cpp         # file:// URL, flags, profile dir, launch spec
    edge_controller.h/.cpp   # EdgeLauncher interface and lifecycle
    compat_flags.h/.cpp      # owned flags, lock line, legacy logs
  tests/unit/
    cover_timeline_test.cpp
    cover_controller_test.cpp
    focus_model_test.cpp
    focus_controller_test.cpp
    edge_spec_test.cpp
    edge_controller_test.cpp
    compat_flags_test.cpp
```

The existing `CONFIGURE_DEPENDS` globs pick up new core sources and tests; `native/CMakeLists.txt` needs no changes.

---

## Task 1: Cover timeline, overlay interface, and controller

**Files:**
- Create: `native/src/core/cover_overlay.h`
- Create: `native/src/core/cover_timeline.h`
- Create: `native/src/core/cover_timeline.cpp`
- Create: `native/src/core/cover_controller.h`
- Create: `native/src/core/cover_controller.cpp`
- Create: `native/tests/unit/cover_timeline_test.cpp`
- Create: `native/tests/unit/cover_controller_test.cpp`

**Interfaces:**
- Produces: `CoverPhase`, `CoverTiming`, `CoverTick`, `CoverTimeline`
- Produces: `CoverOverlay`, `CoverController`

- [ ] **Step 1: Write the failing timeline tests**

Create `native/tests/unit/cover_timeline_test.cpp`:

```cpp
#include "test_main.h"

#include "core/clock.h"
#include "core/cover_timeline.h"

using htmlppt::CoverPhase;
using htmlppt::CoverTick;
using htmlppt::CoverTimeline;
using htmlppt::CoverTiming;
using htmlppt::Duration;
using htmlppt::FakeClock;

HTMLPPT_TEST_CASE(cover_fades_in_and_reports_hold) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  CoverTick tick = timeline.Tick(start + Duration{150});
  HTMLPPT_CHECK_EQ(tick.opacity > 0.49 && tick.opacity < 0.51, true);
  HTMLPPT_CHECK(!tick.hold_reached);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeIn);

  tick = timeline.Tick(start + Duration{300});
  HTMLPPT_CHECK_EQ(tick.opacity, 1.0);
  HTMLPPT_CHECK(tick.hold_reached);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Hold);
}

HTMLPPT_TEST_CASE(cover_waits_for_minimum_hold_before_fading_out) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{300});
  timeline.FadeOut(start + Duration{500});
  CoverTick tick = timeline.Tick(start + Duration{900});
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Hold);
  HTMLPPT_CHECK_EQ(tick.opacity, 1.0);

  tick = timeline.Tick(start + Duration{1300});
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeOut);
  tick = timeline.Tick(start + Duration{1600});
  HTMLPPT_CHECK_EQ(tick.opacity, 0.0);
  HTMLPPT_CHECK(tick.done);
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Idle);
}

HTMLPPT_TEST_CASE(cover_max_hold_forces_fade_out) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{2000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{300});
  CoverTick tick = timeline.Tick(start + Duration{2300});
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::FadeOut);
  tick = timeline.Tick(start + Duration{2600});
  HTMLPPT_CHECK(tick.done);
}

HTMLPPT_TEST_CASE(cover_hide_resets_to_idle) {
  FakeClock clock;
  const auto start = clock.now();
  CoverTimeline timeline(CoverTiming{Duration{300}, Duration{1000}, Duration{10000}});

  timeline.FadeIn(start);
  timeline.Tick(start + Duration{150});
  timeline.Hide();
  HTMLPPT_CHECK_EQ(timeline.phase(), CoverPhase::Idle);
  HTMLPPT_CHECK_EQ(timeline.opacity(), 0.0);
  HTMLPPT_CHECK(!timeline.NextDeadline().has_value());
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: FAIL because `core/cover_timeline.h` does not exist.

- [ ] **Step 3: Implement the cover timeline and overlay interface**

Create `native/src/core/cover_overlay.h`:

```cpp
#pragma once

namespace htmlppt {

class CoverOverlay {
 public:
  virtual ~CoverOverlay() = default;

  virtual bool Show() = 0;
  virtual void Hide() = 0;
  virtual bool SetOpacity(double opacity) = 0;
  virtual void ReassertTopMost() = 0;
};

} // namespace htmlppt
```

Create `native/src/core/cover_timeline.h`:

```cpp
#pragma once

#include <optional>

#include "core/clock.h"

namespace htmlppt {

enum class CoverPhase {
  Idle,
  FadeIn,
  Hold,
  FadeOut,
};

struct CoverTiming {
  Duration fade{300};
  Duration hold{1000};
  Duration max_hold{10000};
};

struct CoverTick {
  double opacity = 0.0;
  bool hold_reached = false;
  bool done = false;
};

class CoverTimeline {
 public:
  explicit CoverTimeline(CoverTiming timing);

  void FadeIn(ClockPoint now);
  void FadeOut(ClockPoint now);
  void Hide();

  CoverTick Tick(ClockPoint now);

  CoverPhase phase() const { return phase_; }
  double opacity() const { return opacity_; }
  std::optional<ClockPoint> NextDeadline() const;

 private:
  double OpacityFor(ClockPoint now, Duration duration) const;

  CoverTiming timing_;
  CoverPhase phase_ = CoverPhase::Idle;
  ClockPoint phase_start_{};
  ClockPoint hold_start_{};
  double opacity_ = 0.0;
  bool fade_out_pending_ = false;
};

} // namespace htmlppt
```

Create `native/src/core/cover_timeline.cpp`:

```cpp
#include "core/cover_timeline.h"

#include <algorithm>

namespace htmlppt {
namespace {

double Ratio(ClockPoint now, ClockPoint start, Duration duration) {
  if (duration.count() <= 0) return 1.0;
  const auto elapsed = std::chrono::duration_cast<Duration>(now - start).count();
  if (elapsed <= 0) return 0.0;
  return std::min(1.0, static_cast<double>(elapsed) /
                           static_cast<double>(duration.count()));
}

} // namespace

CoverTimeline::CoverTimeline(CoverTiming timing) : timing_(timing) {}

void CoverTimeline::FadeIn(ClockPoint now) {
  phase_ = CoverPhase::FadeIn;
  phase_start_ = now;
  opacity_ = 0.0;
  fade_out_pending_ = false;
}

void CoverTimeline::FadeOut(ClockPoint now) {
  if (phase_ == CoverPhase::Idle) return;
  fade_out_pending_ = true;
  if (phase_ == CoverPhase::Hold) {
    // Tick will start the fade once the minimum hold has elapsed.
    (void)now;
  }
}

void CoverTimeline::Hide() {
  phase_ = CoverPhase::Idle;
  opacity_ = 0.0;
  fade_out_pending_ = false;
}

CoverTick CoverTimeline::Tick(ClockPoint now) {
  CoverTick tick;
  tick.opacity = opacity_;

  switch (phase_) {
    case CoverPhase::Idle:
      tick.opacity = 0.0;
      return tick;
    case CoverPhase::FadeIn: {
      tick.opacity = Ratio(now, phase_start_, timing_.fade);
      opacity_ = tick.opacity;
      if (tick.opacity >= 1.0) {
        phase_ = CoverPhase::Hold;
        hold_start_ = now;
        opacity_ = 1.0;
        tick.opacity = 1.0;
        tick.hold_reached = true;
      }
      return tick;
    }
    case CoverPhase::Hold: {
      tick.opacity = 1.0;
      const auto held =
          std::chrono::duration_cast<Duration>(now - hold_start_).count();
      const bool hold_done = held >= timing_.hold.count();
      const bool max_done = held >= timing_.max_hold.count();
      if ((fade_out_pending_ && hold_done) || max_done) {
        phase_ = CoverPhase::FadeOut;
        phase_start_ = now;
        fade_out_pending_ = false;
      }
      return tick;
    }
    case CoverPhase::FadeOut: {
      const double ratio = Ratio(now, phase_start_, timing_.fade);
      tick.opacity = 1.0 - ratio;
      opacity_ = tick.opacity;
      if (ratio >= 1.0) {
        phase_ = CoverPhase::Idle;
        opacity_ = 0.0;
        tick.opacity = 0.0;
        tick.done = true;
      }
      return tick;
    }
  }
  return tick;
}

std::optional<ClockPoint> CoverTimeline::NextDeadline() const {
  switch (phase_) {
    case CoverPhase::Idle:
      return std::nullopt;
    case CoverPhase::FadeIn:
      return phase_start_ + timing_.fade;
    case CoverPhase::Hold: {
      const auto hold_deadline = hold_start_ + timing_.hold;
      const auto max_deadline = hold_start_ + timing_.max_hold;
      if (fade_out_pending_) return hold_deadline;
      return max_deadline;
    }
    case CoverPhase::FadeOut:
      return phase_start_ + timing_.fade;
  }
  return std::nullopt;
}

} // namespace htmlppt
```

Create `native/src/core/cover_controller.h`:

```cpp
#pragma once

#include "core/clock.h"
#include "core/cover_overlay.h"
#include "core/cover_timeline.h"

namespace htmlppt {

class CoverController {
 public:
  CoverController(CoverTiming timing, Clock& clock, CoverOverlay& overlay);

  bool FadeIn();
  void FadeOut();
  void Hide();
  CoverTick Tick();

 private:
  CoverTimeline timeline_;
  Clock& clock_;
  CoverOverlay& overlay_;
  bool visible_ = false;
};

} // namespace htmlppt
```

Create `native/src/core/cover_controller.cpp`:

```cpp
#include "core/cover_controller.h"

namespace htmlppt {

CoverController::CoverController(CoverTiming timing, Clock& clock,
                                 CoverOverlay& overlay)
    : timeline_(timing), clock_(clock), overlay_(overlay) {}

bool CoverController::FadeIn() {
  timeline_.FadeIn(clock_.now());
  visible_ = overlay_.Show();
  if (visible_) overlay_.SetOpacity(0.0);
  return visible_;
}

void CoverController::FadeOut() {
  timeline_.FadeOut(clock_.now());
}

void CoverController::Hide() {
  timeline_.Hide();
  overlay_.Hide();
  visible_ = false;
}

CoverTick CoverController::Tick() {
  CoverTick tick = timeline_.Tick(clock_.now());
  if (visible_) {
    overlay_.SetOpacity(tick.opacity);
    if (tick.hold_reached) overlay_.ReassertTopMost();
    if (tick.done) {
      overlay_.Hide();
      visible_ = false;
    }
  }
  return tick;
}

} // namespace htmlppt
```

- [ ] **Step 4: Write the failing controller tests**

Create `native/tests/unit/cover_controller_test.cpp`:

```cpp
#include "test_main.h"

#include <vector>

#include "core/clock.h"
#include "core/cover_controller.h"
#include "core/cover_overlay.h"

using htmlppt::CoverController;
using htmlppt::CoverOverlay;
using htmlppt::CoverTiming;
using htmlppt::Duration;
using htmlppt::FakeClock;

namespace {

class FakeOverlay final : public CoverOverlay {
 public:
  bool Show() override {
    show_count++;
    visible = true;
    return true;
  }
  void Hide() override {
    hide_count++;
    visible = false;
  }
  bool SetOpacity(double value) override {
    opacity = value;
    return true;
  }
  void ReassertTopMost() override { topmost_count++; }

  int show_count = 0;
  int hide_count = 0;
  int topmost_count = 0;
  bool visible = false;
  double opacity = 0.0;
};

} // namespace

HTMLPPT_TEST_CASE(cover_controller_drives_overlay) {
  FakeClock clock;
  FakeOverlay overlay;
  CoverController controller(
      CoverTiming{Duration{300}, Duration{1000}, Duration{10000}}, clock,
      overlay);

  const auto start = clock.now();
  HTMLPPT_CHECK(controller.FadeIn());
  HTMLPPT_CHECK_EQ(overlay.show_count, 1);
  HTMLPPT_CHECK_EQ(overlay.opacity, 0.0);

  clock.advance(Duration{150});
  auto tick = controller.Tick();
  HTMLPPT_CHECK_EQ(tick.opacity > 0.49 && tick.opacity < 0.51, true);
  HTMLPPT_CHECK_EQ(overlay.opacity > 0.49 && overlay.opacity < 0.51, true);

  clock.advance(Duration{150});
  tick = controller.Tick();
  HTMLPPT_CHECK(tick.hold_reached);
  HTMLPPT_CHECK_EQ(overlay.topmost_count, 1);

  controller.FadeOut();
  clock.advance(Duration{1300});
  tick = controller.Tick();
  HTMLPPT_CHECK(!tick.done);
  clock.advance(Duration{300});
  tick = controller.Tick();
  HTMLPPT_CHECK(tick.done);
  HTMLPPT_CHECK_EQ(overlay.hide_count, 1);
  HTMLPPT_CHECK(!overlay.visible);
}

HTMLPPT_TEST_CASE(cover_controller_hide_is_immediate) {
  FakeClock clock;
  FakeOverlay overlay;
  CoverController controller(
      CoverTiming{Duration{300}, Duration{1000}, Duration{10000}}, clock,
      overlay);
  controller.FadeIn();
  controller.Hide();
  HTMLPPT_CHECK_EQ(overlay.hide_count, 1);
  HTMLPPT_CHECK(!overlay.visible);
}
```

- [ ] **Step 5: Build and run the tests**

Run:

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/htmlppt_core_tests
```

Expected: the six new cover tests pass; all existing tests pass.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/cover_overlay.h native/src/core/cover_timeline.h native/src/core/cover_timeline.cpp native/src/core/cover_controller.h native/src/core/cover_controller.cpp native/tests/unit/cover_timeline_test.cpp native/tests/unit/cover_controller_test.cpp
git commit -m "feat: add cover timeline and overlay controller"
```

---

## Task 2: Window system abstraction and focus model/controller

**Files:**
- Create: `native/src/core/window_system.h`
- Create: `native/src/core/focus_model.h`
- Create: `native/src/core/focus_model.cpp`
- Create: `native/src/core/focus_controller.h`
- Create: `native/src/core/focus_controller.cpp`
- Create: `native/tests/unit/focus_model_test.cpp`
- Create: `native/tests/unit/focus_controller_test.cpp`

**Interfaces:**
- Produces: `WindowId`, `WindowRect`, `WindowInfo`, `ScreenMetrics`, `WindowSystem`
- Produces: `kReturnTitleMarker`, `IsReturnTitle`, `SelectEdgeWindow`, `SelectWpsShowWindow`, `ScoreWpsWindow`
- Produces: `FocusTiming`, `FocusController`

- [ ] **Step 1: Write the failing focus model tests**

Create `native/tests/unit/focus_model_test.cpp`:

```cpp
#include "test_main.h"

#include <string>
#include <utility>
#include <vector>

#include "core/focus_model.h"

using htmlppt::IsReturnTitle;
using htmlppt::ScreenMetrics;
using htmlppt::SelectEdgeWindow;
using htmlppt::SelectWpsShowWindow;
using htmlppt::WindowInfo;
using htmlppt::WindowRect;

namespace {

WindowInfo MakeWindow(htmlppt::WindowId id, std::uint32_t pid,
                      std::string class_name, std::string title, int w, int h,
                      bool popup, bool min_box, bool max_box) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = std::move(class_name);
  window.title = std::move(title);
  window.rect = WindowRect{0, 0, w, h};
  window.visible = true;
  window.ws_popup = popup;
  window.has_min_box = min_box;
  window.has_max_box = max_box;
  return window;
}

std::string WpsShowTitle() {
  return std::string("\xE5\xB9\xBB" "\xE7\x81\xAF" "\xE7\x89\x87"
                     "\xE6\x94\xBE" "\xE6\x98\xA0");
}

} // namespace

HTMLPPT_TEST_CASE(focus_selects_largest_edge_window_for_pid) {
  std::vector<WindowInfo> windows{
      MakeWindow(1, 100, "Chrome_WidgetWin_1", "TEF", 800, 600, true, false, false),
      MakeWindow(2, 100, "Chrome_WidgetWin_1", "TEF", 1920, 1080, true, false, false),
      MakeWindow(3, 200, "Chrome_WidgetWin_1", "Other", 1920, 1080, true, false, false),
  };
  const auto selected = SelectEdgeWindow(windows, 100);
  HTMLPPT_CHECK(selected.has_value());
  HTMLPPT_CHECK_EQ(*selected, htmlppt::WindowId{2});
}

HTMLPPT_TEST_CASE(focus_ranks_wps_show_window) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(1, 300, "Widget", "Document", 1600, 900, false, true, true),
      MakeWindow(2, 300, "screenClass", WpsShowTitle(), 1920, 1080, true, false, false),
      MakeWindow(3, 300, "Widget", "Slide Show", 1900, 1040, true, false, false),
  };
  const auto selected = SelectWpsShowWindow(windows, {300}, screen);
  HTMLPPT_CHECK(selected.has_value());
  HTMLPPT_CHECK_EQ(*selected, htmlppt::WindowId{2});
}

HTMLPPT_TEST_CASE(focus_rejects_wps_windows_without_show_traits) {
  const ScreenMetrics screen{1920, 1080};
  std::vector<WindowInfo> windows{
      MakeWindow(1, 300, "Widget", "Document", 1920, 1080, false, true, true),
  };
  HTMLPPT_CHECK(!SelectWpsShowWindow(windows, {300}, screen).has_value());
}

HTMLPPT_TEST_CASE(focus_recognizes_return_title) {
  HTMLPPT_CHECK(IsReturnTitle("TEF-LIVE-RETURN"));
  HTMLPPT_CHECK(IsReturnTitle("TEF-LIVE-RETURN - demo"));
  HTMLPPT_CHECK(!IsReturnTitle("TEF live"));
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run the same configure/build; expected FAIL because `core/focus_model.h` does not exist.

- [ ] **Step 3: Implement the window system and focus model**

Create `native/src/core/window_system.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace htmlppt {

using WindowId = std::uintptr_t;

struct WindowRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
  long long area() const {
    return static_cast<long long>(width()) * static_cast<long long>(height());
  }
};

struct WindowInfo {
  WindowId id = 0;
  std::uint32_t pid = 0;
  std::string class_name;
  std::string title;
  WindowRect rect;
  bool visible = false;
  bool topmost = false;
  bool ws_popup = false;
  bool has_min_box = true;
  bool has_max_box = true;
};

struct ScreenMetrics {
  int width = 0;
  int height = 0;
};

class WindowSystem {
 public:
  virtual ~WindowSystem() = default;

  virtual std::vector<WindowInfo> Enumerate() = 0;
  virtual WindowId Foreground() = 0;
  virtual bool IsForeground(WindowId id) = 0;
  virtual bool Activate(WindowId id) = 0;
  virtual bool IsWindow(WindowId id) = 0;
};

} // namespace htmlppt
```

Create `native/src/core/focus_model.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/window_system.h"

namespace htmlppt {

inline constexpr std::string_view kReturnTitleMarker = "TEF-LIVE-RETURN";

bool IsReturnTitle(std::string_view title);

std::optional<WindowId> SelectEdgeWindow(const std::vector<WindowInfo>& windows,
                                         std::uint32_t edge_pid);

int ScoreWpsWindow(const WindowInfo& window, ScreenMetrics screen);

std::optional<WindowId> SelectWpsShowWindow(
    const std::vector<WindowInfo>& windows,
    const std::vector<std::uint32_t>& wps_pids, ScreenMetrics screen);

} // namespace htmlppt
```

Create `native/src/core/focus_model.cpp`:

```cpp
#include "core/focus_model.h"

namespace htmlppt {
namespace {

bool Contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

} // namespace

bool IsReturnTitle(std::string_view title) {
  return title.find(kReturnTitleMarker) != std::string_view::npos;
}

std::optional<WindowId> SelectEdgeWindow(const std::vector<WindowInfo>& windows,
                                         std::uint32_t edge_pid) {
  std::optional<WindowId> best;
  long long best_area = -1;
  for (const auto& window : windows) {
    if (window.pid != edge_pid || !window.visible) continue;
    const long long area = window.rect.area();
    if (area > best_area) {
      best_area = area;
      best = window.id;
    }
  }
  return best;
}

int ScoreWpsWindow(const WindowInfo& window, ScreenMetrics screen) {
  if (!window.visible) return 0;
  int score = 0;
  if (window.class_name == "screenClass" ||
      window.class_name == "SlideShowClass" ||
      window.class_name == "Widget") {
    score += 2;
  }
  // UTF-8 for the WPS slideshow title word.
  static constexpr char kWpsTitleWord[] =
      "\xE5\xB9\xBB" "\xE7\x81\xAF" "\xE7\x89\x87";
  if (Contains(window.title, kWpsTitleWord)) score += 3;
  if (Contains(window.title, "Slide Show") ||
      Contains(window.title, "Slideshow")) {
    score += 3;
  }
  if (window.ws_popup) score += 2;
  if (!window.has_min_box && !window.has_max_box) score += 2;
  if (screen.width > 0 && screen.height > 0) {
    const int min_width = static_cast<int>(screen.width * 0.9);
    const int min_height = static_cast<int>(screen.height * 0.9);
    if (window.rect.width() >= min_width && window.rect.height() >= min_height) {
      score += 1;
    }
  }
  return score;
}

std::optional<WindowId> SelectWpsShowWindow(
    const std::vector<WindowInfo>& windows,
    const std::vector<std::uint32_t>& wps_pids, ScreenMetrics screen) {
  std::optional<WindowId> best;
  int best_score = 0;
  for (const auto& window : windows) {
    if (!window.visible) continue;
    bool pid_match = false;
    for (const auto pid : wps_pids) {
      if (window.pid == pid) {
        pid_match = true;
        break;
      }
    }
    if (!pid_match) continue;
    const int score = ScoreWpsWindow(window, screen);
    // Require at least one show trait; a maximized editor scores 3.
    if (score >= 4 && score > best_score) {
      best_score = score;
      best = window.id;
    }
  }
  return best;
}

} // namespace htmlppt
```

- [ ] **Step 4: Implement the focus controller**

Create `native/src/core/focus_controller.h`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include "core/clock.h"
#include "core/window_system.h"

namespace htmlppt {

struct FocusTiming {
  Duration interval{100};
  Duration timeout{6000};
};

class FocusController {
 public:
  FocusController(WindowSystem& windows, Clock& clock, FocusTiming timing = {});

  bool FocusEdge(std::uint32_t edge_pid);
  bool FocusWps(const std::vector<std::uint32_t>& wps_pids,
                ScreenMetrics screen);
  bool IsReturnSignal(std::uint32_t edge_pid);

 private:
  bool FocusWindow(WindowId id);

  WindowSystem& windows_;
  Clock& clock_;
  FocusTiming timing_;
};

} // namespace htmlppt
```

Create `native/src/core/focus_controller.cpp`:

```cpp
#include "core/focus_controller.h"

#include "core/focus_model.h"

namespace htmlppt {

FocusController::FocusController(WindowSystem& windows, Clock& clock,
                                 FocusTiming timing)
    : windows_(windows), clock_(clock), timing_(timing) {}

bool FocusController::FocusWindow(WindowId id) {
  if (!windows_.Activate(id)) return false;
  return windows_.IsForeground(id);
}

bool FocusController::FocusEdge(std::uint32_t edge_pid) {
  const auto deadline = clock_.now() + timing_.timeout;
  while (clock_.now() < deadline) {
    const auto selected = SelectEdgeWindow(windows_.Enumerate(), edge_pid);
    if (selected.has_value() && FocusWindow(*selected)) return true;
    clock_.sleep_until(clock_.now() + timing_.interval);
  }
  return false;
}

bool FocusController::FocusWps(const std::vector<std::uint32_t>& wps_pids,
                               ScreenMetrics screen) {
  const auto deadline = clock_.now() + timing_.timeout;
  while (clock_.now() < deadline) {
    const auto selected =
        SelectWpsShowWindow(windows_.Enumerate(), wps_pids, screen);
    if (selected.has_value() && FocusWindow(*selected)) return true;
    clock_.sleep_until(clock_.now() + timing_.interval);
  }
  return false;
}

bool FocusController::IsReturnSignal(std::uint32_t edge_pid) {
  for (const auto& window : windows_.Enumerate()) {
    if (window.pid == edge_pid && IsReturnTitle(window.title)) return true;
  }
  return false;
}

} // namespace htmlppt
```

- [ ] **Step 5: Write the failing focus controller tests**

Create `native/tests/unit/focus_controller_test.cpp`:

```cpp
#include "test_main.h"

#include <cstdint>
#include <vector>

#include "core/clock.h"
#include "core/focus_controller.h"
#include "core/window_system.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::FocusController;
using htmlppt::FocusTiming;
using htmlppt::ScreenMetrics;
using htmlppt::WindowId;
using htmlppt::WindowInfo;
using htmlppt::WindowRect;
using htmlppt::WindowSystem;

namespace {

class FakeWindows final : public WindowSystem {
 public:
  std::vector<WindowInfo> Enumerate() override { return windows; }
  WindowId Foreground() override { return foreground; }
  bool IsForeground(WindowId id) override { return id == foreground; }
  bool Activate(WindowId id) override {
    if (!allow_activate) return false;
    foreground = id;
    return true;
  }
  bool IsWindow(WindowId id) override {
    for (const auto& window : windows) {
      if (window.id == id) return true;
    }
    return false;
  }

  std::vector<WindowInfo> windows;
  WindowId foreground = 0;
  bool allow_activate = true;
};

WindowInfo MakeEdge(WindowId id, std::uint32_t pid, int width, int height) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = "Chrome_WidgetWin_1";
  window.title = "TEF";
  window.rect = WindowRect{0, 0, width, height};
  window.visible = true;
  return window;
}

WindowInfo MakeWps(WindowId id, std::uint32_t pid, bool return_title = false) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.class_name = "screenClass";
  window.title = return_title ? "TEF-LIVE-RETURN" : "Slide Show";
  window.rect = WindowRect{0, 0, 1920, 1080};
  window.visible = true;
  window.ws_popup = true;
  window.has_min_box = false;
  window.has_max_box = false;
  return window;
}

} // namespace

HTMLPPT_TEST_CASE(focus_controller_activates_edge_window) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600), MakeEdge(2, 100, 1920, 1080)};
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(controller.FocusEdge(100));
  HTMLPPT_CHECK_EQ(windows.foreground, WindowId{2});
}

HTMLPPT_TEST_CASE(focus_controller_times_out_when_activation_is_denied) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeEdge(1, 100, 800, 600)};
  windows.allow_activate = false;
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{50}});
  HTMLPPT_CHECK(!controller.FocusEdge(100));
}

HTMLPPT_TEST_CASE(focus_controller_activates_wps_show_window) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeWps(1, 300)};
  FocusController controller(windows, clock, FocusTiming{Duration{10}, Duration{100}});
  HTMLPPT_CHECK(controller.FocusWps({300}, ScreenMetrics{1920, 1080}));
  HTMLPPT_CHECK_EQ(windows.foreground, WindowId{1});
}

HTMLPPT_TEST_CASE(focus_controller_detects_return_title) {
  FakeClock clock;
  FakeWindows windows;
  windows.windows = {MakeWps(1, 100, true)};
  FocusController controller(windows, clock);
  HTMLPPT_CHECK(controller.IsReturnSignal(100));
  HTMLPPT_CHECK(!controller.IsReturnSignal(200));
}
```

- [ ] **Step 6: Build and run the tests**

Run the configure/build/CTest/unit commands. Expected: all cover and focus tests pass.

- [ ] **Step 7: Commit**

```bash
git add native/src/core/window_system.h native/src/core/focus_model.h native/src/core/focus_model.cpp native/src/core/focus_controller.h native/src/core/focus_controller.cpp native/tests/unit/focus_model_test.cpp native/tests/unit/focus_controller_test.cpp
git commit -m "feat: add window focus model and controller"
```

---

## Task 3: Edge launch specification and lifecycle controller

**Files:**
- Create: `native/src/core/edge_spec.h`
- Create: `native/src/core/edge_spec.cpp`
- Create: `native/src/core/edge_controller.h`
- Create: `native/src/core/edge_controller.cpp`
- Create: `native/tests/unit/edge_spec_test.cpp`
- Create: `native/tests/unit/edge_controller_test.cpp`

**Interfaces:**
- Produces: `EdgeLaunchSpec`, `BuildEdgeUrl`, `DefaultEdgeFlags`, `MakeEdgeLaunchSpec`
- Produces: `EdgeProcess`, `EdgeLauncher`, `EdgeController`

- [ ] **Step 1: Write the failing Edge spec tests**

Create `native/tests/unit/edge_spec_test.cpp`:

```cpp
#include "test_main.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/edge_spec.h"

using htmlppt::BuildEdgeUrl;
using htmlppt::DefaultEdgeFlags;
using htmlppt::EdgeConfig;
using htmlppt::EdgeLaunchSpec;
using htmlppt::MakeEdgeLaunchSpec;
using htmlppt::ResolvedTarget;

HTMLPPT_TEST_CASE(edge_url_encodes_unicode_and_spaces) {
  ResolvedTarget target;
  std::string path = "/tmp/";
  path += "\xE6\xBC\x94";  // UTF-8 for a CJK character.
  path += "\xE7\xA4\xBA";
  path += " page.html";
  target.html_path = path;
  target.query = "lang=zh";
  target.fragment = "step2";
  const std::string url = BuildEdgeUrl(target);
  HTMLPPT_CHECK_EQ(url.rfind("file:///", 0), 0);
  HTMLPPT_CHECK(url.find("%20") != std::string::npos);
  HTMLPPT_CHECK(url.find("%E6%BC%94") != std::string::npos);
  HTMLPPT_CHECK(url.find("?lang=zh") != std::string::npos);
  HTMLPPT_CHECK(url.find("#step2") != std::string::npos);
}

HTMLPPT_TEST_CASE(edge_url_keeps_query_and_fragment_empty) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  const std::string url = BuildEdgeUrl(target);
  HTMLPPT_CHECK_EQ(url, std::string{"file:///tmp/page1.html"});
}

HTMLPPT_TEST_CASE(edge_spec_uses_profile_and_default_flags) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  target.edge_profile = "htmlppt-deck-t05";
  const EdgeLaunchSpec spec = MakeEdgeLaunchSpec(
      "/usr/bin/msedge", "/tmp", target, EdgeConfig{});
  HTMLPPT_CHECK_EQ(spec.executable.string(), std::string{"/usr/bin/msedge"});
  HTMLPPT_CHECK_EQ(spec.profile_dir.string(),
                   std::string{"/tmp/htmlppt-deck-t05"});
  bool has_app = false;
  bool has_profile = false;
  bool has_fullscreen = false;
  for (const auto& arg : spec.arguments) {
    if (arg.rfind("--app=", 0) == 0) has_app = true;
    if (arg.rfind("--user-data-dir=", 0) == 0) has_profile = true;
    if (arg == "--start-fullscreen") has_fullscreen = true;
  }
  HTMLPPT_CHECK(has_app);
  HTMLPPT_CHECK(has_profile);
  HTMLPPT_CHECK(has_fullscreen);
  HTMLPPT_CHECK(!DefaultEdgeFlags().empty());
}

HTMLPPT_TEST_CASE(edge_spec_uses_configured_flags_when_present) {
  ResolvedTarget target;
  target.html_path = "/tmp/page1.html";
  target.edge_profile = "profile";
  EdgeConfig config;
  config.args = {"--custom-flag"};
  const EdgeLaunchSpec spec =
      MakeEdgeLaunchSpec("/usr/bin/msedge", "/tmp", target, config);
  bool found = false;
  for (const auto& arg : spec.arguments) {
    if (arg == "--custom-flag") found = true;
  }
  HTMLPPT_CHECK(found);
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Expected: FAIL because `core/edge_spec.h` does not exist.

- [ ] **Step 3: Implement the Edge specification**

Create `native/src/core/edge_spec.h`:

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/resolve.h"

namespace htmlppt {

struct EdgeLaunchSpec {
  std::filesystem::path executable;
  std::string url;
  std::filesystem::path profile_dir;
  std::vector<std::string> arguments;
};

std::string BuildEdgeUrl(const ResolvedTarget& target);
std::vector<std::string> DefaultEdgeFlags();
EdgeLaunchSpec MakeEdgeLaunchSpec(const std::filesystem::path& edge_exe,
                                  const std::filesystem::path& temp_dir,
                                  const ResolvedTarget& target,
                                  const EdgeConfig& config);

} // namespace htmlppt
```

Create `native/src/core/edge_spec.cpp`:

```cpp
#include "core/edge_spec.h"

#include <algorithm>
#include <cctype>

namespace htmlppt {
namespace {

bool IsSafePathByte(unsigned char value) {
  return std::isalnum(value) != 0 || value == '/' || value == ':' ||
         value == '-' || value == '.' || value == '_' || value == '~';
}

std::string PercentEncodePath(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  std::string encoded;
  encoded.reserve(text.size() * 3);
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char value : text) {
    if (IsSafePathByte(value)) {
      encoded.push_back(static_cast<char>(value));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[value >> 4]);
      encoded.push_back(kHex[value & 0x0F]);
    }
  }
  return encoded;
}

} // namespace

std::string BuildEdgeUrl(const ResolvedTarget& target) {
  std::string path = target.html_path;
  std::replace(path.begin(), path.end(), '\\', '/');
  std::string url;
  if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    url = "file:///" + PercentEncodePath(path);
  } else if (!path.empty() && path.front() == '/') {
    url = "file://" + PercentEncodePath(path);
  } else {
    url = "file:///" + PercentEncodePath(path);
  }
  if (!target.query.empty()) url += "?" + target.query;
  if (!target.fragment.empty()) url += "#" + target.fragment;
  return url;
}

std::vector<std::string> DefaultEdgeFlags() {
  return {
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-sync",
      "--disable-features=msImplicitSignin,msEdgeSync,msFirstRunExperience,"
      "msEdgeFirstRunExperience,EdgeSync",
      "--disable-background-networking",
      "--disable-component-update",
      "--disable-extensions",
      "--disable-default-apps",
      "--no-service-autorun",
      "--disable-client-side-phishing-detection",
      "--disable-domain-reliability",
      "--no-pings",
      "--disable-breakpad",
  };
}

EdgeLaunchSpec MakeEdgeLaunchSpec(const std::filesystem::path& edge_exe,
                                  const std::filesystem::path& temp_dir,
                                  const ResolvedTarget& target,
                                  const EdgeConfig& config) {
  EdgeLaunchSpec spec;
  spec.executable = edge_exe;
  spec.url = BuildEdgeUrl(target);
  spec.profile_dir = temp_dir / target.edge_profile;
  spec.arguments = config.args.empty() ? DefaultEdgeFlags() : config.args;
  spec.arguments.push_back("--app=" + spec.url);
  spec.arguments.push_back("--start-fullscreen");
  spec.arguments.push_back("--user-data-dir=" + spec.profile_dir.string());
  return spec;
}

} // namespace htmlppt
```

- [ ] **Step 4: Implement the Edge controller**

Create `native/src/core/edge_controller.h`:

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
};

class EdgeLauncher {
 public:
  virtual ~EdgeLauncher() = default;
  virtual std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) = 0;
};

class EdgeController {
 public:
  explicit EdgeController(EdgeLauncher& launcher);

  bool Launch(const EdgeLaunchSpec& spec);
  void Kill();
  bool active() const;
  std::uint32_t pid() const;

 private:
  EdgeLauncher& launcher_;
  std::unique_ptr<EdgeProcess> process_;
};

} // namespace htmlppt
```

Create `native/src/core/edge_controller.cpp`:

```cpp
#include "core/edge_controller.h"

namespace htmlppt {

EdgeController::EdgeController(EdgeLauncher& launcher) : launcher_(launcher) {}

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

bool EdgeController::active() const {
  return process_ != nullptr && process_->alive();
}

std::uint32_t EdgeController::pid() const {
  return process_ != nullptr ? process_->pid() : 0;
}

} // namespace htmlppt
```

- [ ] **Step 5: Write the failing Edge controller tests**

Create `native/tests/unit/edge_controller_test.cpp`:

```cpp
#include "test_main.h"

#include <cstdint>
#include <memory>

#include "core/edge_controller.h"

using htmlppt::EdgeController;
using htmlppt::EdgeLaunchSpec;
using htmlppt::EdgeLauncher;
using htmlppt::EdgeProcess;

namespace {

class FakeEdgeProcess final : public EdgeProcess {
 public:
  explicit FakeEdgeProcess(std::uint32_t pid) : pid_(pid) {}
  std::uint32_t pid() const override { return pid_; }
  bool alive() const override { return alive_; }
  void terminate() override { alive_ = false; }

  std::uint32_t pid_ = 0;
  bool alive_ = true;
};

class FakeLauncher final : public EdgeLauncher {
 public:
  std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec&) override {
    ++launch_count;
    if (!succeed) return nullptr;
    return std::make_unique<FakeEdgeProcess>(next_pid++);
  }
  int launch_count = 0;
  std::uint32_t next_pid = 100;
  bool succeed = true;
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
```

- [ ] **Step 6: Build and run the tests**

Run the configure/build/CTest/unit commands. Expected: all Edge tests pass.

- [ ] **Step 7: Commit**

```bash
git add native/src/core/edge_spec.h native/src/core/edge_spec.cpp native/src/core/edge_controller.h native/src/core/edge_controller.cpp native/tests/unit/edge_spec_test.cpp native/tests/unit/edge_controller_test.cpp
git commit -m "feat: add edge launch spec and lifecycle controller"
```

---

## Task 4: Compatibility flags and legacy logs

**Files:**
- Create: `native/src/core/compat_flags.h`
- Create: `native/src/core/compat_flags.cpp`
- Create: `native/tests/unit/compat_flags_test.cpp`

**Interfaces:**
- Produces: flag name constants, `CompatIdentity`, `CompatFlags`, `LegacyLog`, `UtcTimestamp`

- [ ] **Step 1: Write the failing compatibility tests**

Create `native/tests/unit/compat_flags_test.cpp`:

```cpp
#include "test_main.h"
#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/compat_flags.h"

using htmlppt::CompatFlags;
using htmlppt::CompatIdentity;
using htmlppt::LegacyLog;
using htmlppt::test::TempDir;

HTMLPPT_TEST_CASE(compat_owns_and_deletes_its_flags) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  HTMLPPT_CHECK(flags.WriteFlag(htmlppt::kCoverFadeFlag, "payload"));
  HTMLPPT_CHECK_EQ(flags.ReadFlag(htmlppt::kCoverFadeFlag),
                   std::string{"payload"});
  HTMLPPT_CHECK(flags.IsOwned(htmlppt::kCoverFadeFlag));

  CompatFlags other(dir.path(),
                    CompatIdentity{"run-b", 5678, "config-hash"});
  HTMLPPT_CHECK(!other.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!other.DeleteOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(flags.DeleteOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
}

HTMLPPT_TEST_CASE(compat_lock_line_contains_identity) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  HTMLPPT_CHECK(flags.WriteLock("2026-11-15T00:00:00Z"));
  const std::string lock = flags.ReadFlag(htmlppt::kLockFile);
  HTMLPPT_CHECK(lock.find("run-a") != std::string::npos);
  HTMLPPT_CHECK(lock.find("1234") != std::string::npos);
  HTMLPPT_CHECK(lock.find("config-hash") != std::string::npos);
}

HTMLPPT_TEST_CASE(compat_delete_all_owned_flags) {
  TempDir dir;
  CompatFlags flags(dir.path(),
                    CompatIdentity{"run-a", 1234, "config-hash"});
  flags.WriteFlag(htmlppt::kCoverFadeFlag, "");
  flags.WriteFlag(htmlppt::kCoverReadyFlag, "");
  flags.WriteFlag(htmlppt::kAgentStopFlag, "");
  flags.DeleteAllOwned();
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverFadeFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kCoverReadyFlag));
  HTMLPPT_CHECK(!flags.IsOwned(htmlppt::kAgentStopFlag));
}

HTMLPPT_TEST_CASE(legacy_log_appends_timestamped_lines) {
  TempDir dir;
  LegacyLog log(dir.path());
  HTMLPPT_CHECK(log.Append(htmlppt::kWatchLog, "cover started"));
  const auto path = dir.path() / htmlppt::kWatchLog;
  HTMLPPT_CHECK(std::filesystem::exists(path));
  const std::string text = [&] {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  }();
  HTMLPPT_CHECK(text.find("cover started") != std::string::npos);
  HTMLPPT_CHECK(text.find("T") != std::string::npos);
}

HTMLPPT_TEST_CASE(compat_utc_timestamp_is_non_empty) {
  HTMLPPT_CHECK(!htmlppt::UtcTimestamp().empty());
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Expected: FAIL because `core/compat_flags.h` does not exist.

- [ ] **Step 3: Implement compatibility flags and logs**

Create `native/src/core/compat_flags.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace htmlppt {

inline constexpr std::string_view kLockFile = "watcher.lock";
inline constexpr std::string_view kCoverFadeFlag = "cover-fade.flag";
inline constexpr std::string_view kCoverReadyFlag = "cover-ready.flag";
inline constexpr std::string_view kCoverDoneFlag = "cover-done.flag";
inline constexpr std::string_view kCoverStopFlag = "cover-stop.flag";
inline constexpr std::string_view kCoverHideFlag = "cover-hide.flag";
inline constexpr std::string_view kAgentArmFlag = "agent-arm.flag";
inline constexpr std::string_view kAgentStopFlag = "agent-stop.flag";
inline constexpr std::string_view kReturnRequestFlag = "return-request.flag";
inline constexpr std::string_view kEdgeClosedFlag = "edge-closed.flag";
inline constexpr std::string_view kWatchLog = "watch-log.txt";
inline constexpr std::string_view kBringLog = "bring-log.txt";
inline constexpr std::string_view kCoverLog = "cover-log.txt";

struct CompatIdentity {
  std::string run_id;
  std::uint32_t pid = 0;
  std::string config_hash;
};

class CompatFlags {
 public:
  CompatFlags(std::filesystem::path base_dir, CompatIdentity identity);

  const CompatIdentity& identity() const { return identity_; }

  bool WriteLock(std::string_view timestamp);
  bool WriteFlag(std::string_view name, std::string_view payload = {});
  std::string ReadFlag(std::string_view name) const;
  bool IsOwned(std::string_view name) const;
  bool DeleteOwned(std::string_view name);
  void DeleteAllOwned();

 private:
  std::filesystem::path Path(std::string_view name) const;
  std::filesystem::path OwnerPath(std::string_view name) const;
  bool WriteText(const std::filesystem::path& path, std::string_view text) const;
  std::string ReadText(const std::filesystem::path& path) const;

  std::filesystem::path base_dir_;
  CompatIdentity identity_;
};

class LegacyLog {
 public:
  explicit LegacyLog(std::filesystem::path base_dir);

  bool Append(std::string_view file, std::string_view line) const;

 private:
  std::filesystem::path base_dir_;
};

std::string UtcTimestamp();

} // namespace htmlppt
```

Create `native/src/core/compat_flags.cpp`:

```cpp
#include "core/compat_flags.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

namespace htmlppt {

CompatFlags::CompatFlags(std::filesystem::path base_dir,
                         CompatIdentity identity)
    : base_dir_(std::move(base_dir)), identity_(std::move(identity)) {}

std::filesystem::path CompatFlags::Path(std::string_view name) const {
  return base_dir_ / std::string(name);
}

std::filesystem::path CompatFlags::OwnerPath(std::string_view name) const {
  return base_dir_ / (std::string(name) + ".owner");
}

bool CompatFlags::WriteText(const std::filesystem::path& path,
                            std::string_view text) const {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  return output.good();
}

std::string CompatFlags::ReadText(const std::filesystem::path& path) const {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool CompatFlags::WriteFlag(std::string_view name, std::string_view payload) {
  if (!WriteText(Path(name), payload)) return false;
  return WriteText(OwnerPath(name), identity_.run_id);
}

std::string CompatFlags::ReadFlag(std::string_view name) const {
  return ReadText(Path(name));
}

bool CompatFlags::IsOwned(std::string_view name) const {
  return ReadText(OwnerPath(name)) == identity_.run_id;
}

bool CompatFlags::DeleteOwned(std::string_view name) {
  if (!IsOwned(name)) return false;
  std::error_code error;
  std::filesystem::remove(Path(name), error);
  std::filesystem::remove(OwnerPath(name), error);
  return !error;
}

void CompatFlags::DeleteAllOwned() {
  DeleteOwned(kLockFile);
  DeleteOwned(kCoverFadeFlag);
  DeleteOwned(kCoverReadyFlag);
  DeleteOwned(kCoverDoneFlag);
  DeleteOwned(kCoverStopFlag);
  DeleteOwned(kCoverHideFlag);
  DeleteOwned(kAgentArmFlag);
  DeleteOwned(kAgentStopFlag);
  DeleteOwned(kReturnRequestFlag);
  DeleteOwned(kEdgeClosedFlag);
}

bool CompatFlags::WriteLock(std::string_view timestamp) {
  nlohmann::ordered_json line{
      {"run_id", identity_.run_id},
      {"pid", identity_.pid},
      {"config_hash", identity_.config_hash},
      {"updated", std::string(timestamp)},
  };
  return WriteFlag(kLockFile, line.dump());
}

LegacyLog::LegacyLog(std::filesystem::path base_dir)
    : base_dir_(std::move(base_dir)) {}

bool LegacyLog::Append(std::string_view file, std::string_view line) const {
  std::error_code error;
  std::filesystem::create_directories(base_dir_, error);
  std::ofstream output(base_dir_ / std::string(file),
                       std::ios::binary | std::ios::app);
  if (!output) return false;
  output << UtcTimestamp() << ' ' << line << '\n';
  return output.good();
}

std::string UtcTimestamp() {
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

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

Run the configure/build/CTest/unit commands. Expected: all compatibility tests pass.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/compat_flags.h native/src/core/compat_flags.cpp native/tests/unit/compat_flags_test.cpp
git commit -m "feat: add compatibility flags and legacy logs"
```

---

## Task 5: Documentation and full Plan 5B-1 gate

**Files:**
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-15-cpp-watchdog-cover-focus-edge-core.md` (As-Built Amendments)

- [ ] **Step 1: Update README**

Add a section before the WPS COM host section:

```markdown
## Cover, focus, Edge, and compatibility core (Plan 5B-1)

`core/cover_timeline` owns fade-in, minimum hold, maximum hold safety, and
fade-out. `core/cover_controller` drives a `CoverOverlay` implementation and
re-asserts topmost when the hold edge is reached.

`core/window_system` is the platform-neutral window descriptor/control
interface. `core/focus_model` ranks WPS slideshow windows by class, title,
popup style, and fullscreen traits, selects the largest visible Edge window
for a PID, and recognizes `TEF-LIVE-RETURN`. `core/focus_controller` polls
`Activate` until the window is foreground or the timeout expires.

`core/edge_spec` builds a `file://` URL, percent-encodes UTF-8 paths, applies
the default or configured Chromium flags, and assigns a per-target profile
directory. `core/edge_controller` owns launch/active/kill over an
`EdgeLauncher` interface.

`core/compat_flags` writes owner-tracked legacy flags (`watcher.lock`,
`cover-*`, `agent-*`, `return-request`, `edge-closed`) without altering the
legacy payload content; ownership is stored in a sibling `.owner` file.
`core/legacy_log` appends timestamped `watch-log.txt`, `bring-log.txt`, and
`cover-log.txt` lines.

These units are platform-neutral and tested on Linux. Plan 5B-2 maps them onto
Win32 layered windows, `EnumWindows`, foreground unlocking, and
`CreateProcessW`/Job Objects.
```

- [ ] **Step 2: Add As-Built Amendments**

Append `## As-Built Amendments (2026-11-15)` to this plan with: the final unit-test count, the exact gate commands, and either the deviations discovered during implementation or the line `No deviations`. Do not leave the section empty.

- [ ] **Step 3: Run the full gate**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
./native/build/htmlppt_core_tests
npm test
```

Expected: all existing 191 tests plus the new Plan 5B-1 tests pass; CTest 8/8; npm 3/3; `-Werror` clean; working tree clean.

- [ ] **Step 4: Commit**

```bash
git add native/README.md docs/superpowers/plans/2026-11-15-cpp-watchdog-cover-focus-edge-core.md
git commit -m "docs: record Plan 5B-1 cover/focus/edge core"
```

---

## Self-Review

### Plan 5B-1 spec coverage

| Spec requirement | Plan 5B-1 coverage |
|---|---|
| Cover fade/hold/max-hold/fade-out timeline | Task 1 |
| Overlay show/hide/opacity/topmost interface | Task 1 |
| WPS window scoring and Edge window selection | Task 2 |
| Focus activation loop with timeout | Task 2 |
| `TEF-LIVE-RETURN` title recognition | Task 2 |
| Edge `file://` URL, flags, profile, args | Task 3 |
| Edge launch/active/kill lifecycle | Task 3 |
| Legacy flag ownership and mirror content | Task 4 |
| Legacy log append | Task 4 |
| Win32 overlay/window/process mapping | Plan 5B-2 |

### Placeholder scan

- No `TBD`, `TODO`, `implement later`, or empty steps.
- Every code step contains the code to write.
- Every run step contains the exact command and expected result.

### Type consistency

- `Duration`/`ClockPoint` come from `core/clock.h`.
- `ResolvedTarget`/`EdgeConfig` come from `core/resolve.h`/`core/config.h`.
- `WindowId` is `std::uintptr_t`; `WindowInfo` carries the fields the Win32 adapter must fill.
- `CoverOverlay`, `WindowSystem`, `EdgeLauncher` are the only platform seams for Plan 5B-2.

---

## Execution Handoff

Plan 5B-1 is ready. Continue with subagent-driven development: create a fresh worktree from `main`, commit this plan, execute Tasks 1-5 with task-level reviews, then run the final whole-branch review. Plan 5B-2 (Win32 implementations) will be authored after 5B-1 lands.
