#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "core/cover_overlay.h"
#include "core/edge_controller.h"
#include "core/edge_spec.h"
#include "core/presentation_host.h"
#include "core/resolve.h"
#include "core/state_machine_types.h"
#include "core/types.h"
#include "core/window_system.h"
#include "core/worker_plan.h"
#include "core/worker_runtime.h"

namespace htmlppt::test {

// Builders rather than brace initialization: the test target builds with
// -Wextra -Werror, and these structs have members without default member
// initializers (ShowState::detail, the WindowInfo strings and rect, the
// TargetPlan optionals), so a partial braced initializer is rejected.
inline ShowState MakeShowState(HostStatus status, bool active, int slide) {
  ShowState show;
  show.status = status;
  show.active = active;
  show.slide = slide;
  return show;
}

inline WindowInfo MakeWindow(WindowId id, std::uint32_t pid,
                             std::string title) {
  WindowInfo window;
  window.id = id;
  window.pid = pid;
  window.title = std::move(title);
  window.visible = true;
  return window;
}

inline TargetPlan MakeTarget(int slide, std::string label,
                             std::string edge_profile) {
  TargetPlan target;
  target.slide = slide;
  target.label = std::move(label);
  target.edge_profile = std::move(edge_profile);
  return target;
}

// The resolved spec that pairs with a machine target. plan.machine.targets and
// plan.targets are index-aligned, so callers must add both.
inline ResolvedTarget MakeResolvedTarget(int ordinal, int slide,
                                         std::string label,
                                         std::string edge_profile) {
  ResolvedTarget target;
  target.ordinal = ordinal;
  target.slide = slide;
  target.label = std::move(label);
  target.edge_profile = std::move(edge_profile);
  return target;
}

// Records presentation calls and reports the ShowState the test assigns.
class FakePresentationHost final : public PresentationHost {
 public:
  HostResult Launch(const std::filesystem::path&) override { return {}; }
  HostResult StartShow() override {
    ++start_show_calls;
    return {};
  }
  ShowState QueryShow() override { return show; }
  HostResult ExitShow() override {
    ++exit_show_calls;
    if (fail_next_exit) {
      fail_next_exit = false;
      return Failed();
    }
    return {};
  }
  HostResult ResumeShow(int slide) override {
    resume_slides.push_back(slide);
    if (fail_next_resume) {
      fail_next_resume = false;
      return Failed();
    }
    return {};
  }
  HostResult FocusShow() override { return {}; }
  void Release() override {}

  ShowState show;
  int start_show_calls = 0;
  int exit_show_calls = 0;
  std::vector<int> resume_slides;
  // Rejects the next ExitShow/ResumeShow call; consumed after one use so a
  // retry sees a working host again.
  bool fail_next_exit = false;
  bool fail_next_resume = false;

 private:
  static HostResult Failed() {
    HostResult result;
    result.status = HostStatus::Failed;
    result.detail = "fake presentation failure";
    return result;
  }
};

// Records overlay calls and the last opacity the controller applied.
class FakeOverlay final : public CoverOverlay {
 public:
  bool Show() override {
    ++show_calls;
    visible = true;
    return true;
  }
  void Hide() override {
    ++hide_calls;
    visible = false;
  }
  bool SetOpacity(double value) override {
    opacity = value;
    return true;
  }
  void ReassertTopMost() override { ++topmost_calls; }

  int show_calls = 0;
  int hide_calls = 0;
  int topmost_calls = 0;
  bool visible = false;
  double opacity = 0.0;
};

// A launched process whose liveness and kill count are owned by FakeLauncher
// so tests can read them after the controller has released the process.
class FakeEdgeProcess final : public EdgeProcess {
 public:
  FakeEdgeProcess(std::uint32_t pid, bool& alive, int& kill_calls)
      : pid_(pid), alive_(alive), kill_calls_(kill_calls) {}

  std::uint32_t pid() const override { return pid_; }
  bool alive() const override { return alive_; }
  void terminate() override {
    alive_ = false;
    ++kill_calls_;
  }
  bool release() override { return true; }

 private:
  std::uint32_t pid_ = 0;
  bool& alive_;
  int& kill_calls_;
};

class FakeLauncher final : public EdgeLauncher {
 public:
  FakeLauncher() = default;
  FakeLauncher(const FakeLauncher&) = delete;
  FakeLauncher& operator=(const FakeLauncher&) = delete;

  std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec& spec) override {
    ++launch_count;
    last_spec = spec;
    if (fail_next) {
      fail_next = false;
      return nullptr;
    }
    pid = next_pid++;
    alive = true;
    return std::make_unique<FakeEdgeProcess>(pid, alive, kill_calls);
  }

  // Simulates the Edge process exiting on its own.
  void set_alive(bool value) { alive = value; }

  int launch_count = 0;
  int kill_calls = 0;
  EdgeLaunchSpec last_spec;
  std::uint32_t pid = 0;
  std::uint32_t next_pid = 500;
  bool alive = true;
  // Rejects the next Launch call without assigning a pid.
  bool fail_next = false;
};

class FakeWindowSystem final : public WindowSystem {
 public:
  std::vector<WindowInfo> Enumerate() override { return windows; }
  WindowId Foreground() override { return foreground; }
  bool IsForeground(WindowId id) override { return id == foreground; }
  bool Activate(WindowId id) override {
    activate_calls.push_back(id);
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
  std::vector<WindowId> activate_calls;
  WindowId foreground = 0;
};

struct TriggeredAtSlide1Tag {};
struct LiveAtSlide1Tag {};

// Owns every port fake, the clock, the plan/config, and the runtime. The
// runtime holds references to these members, so the harness is neither
// copyable nor movable and is only created in place.
class WorkerHarness {
 public:
  WorkerHarness()
      : runtime(plan, config,
                WorkerPorts{presentation, overlay, launcher, windows, clock},
                host) {}
  explicit WorkerHarness(TriggeredAtSlide1Tag);
  explicit WorkerHarness(LiveAtSlide1Tag);

  WorkerHarness(const WorkerHarness&) = delete;
  WorkerHarness& operator=(const WorkerHarness&) = delete;

  void Start() { runtime.Start(); }
  bool Step() { return runtime.Step(); }
  void advance(Duration delta) { clock.advance(delta); }

  // Re-creates the runtime in place after a test mutates plan/config.
  void rebuild() {
    std::destroy_at(&runtime);
    std::construct_at(
        &runtime, plan, config,
        WorkerPorts{presentation, overlay, launcher, windows, clock}, host);
  }

  FakePresentationHost presentation;
  FakeOverlay overlay;
  FakeLauncher launcher;
  FakeWindowSystem windows;
  FakeClock clock;
  WorkerPlan plan;
  StateMachineConfig config;
  WorkerHostConfig host;
  WorkerRuntime runtime;
};

// Seeds a target at slide 1 and leaves the machine in CoverIn with the cover
// fading in: Start + one Step reach Monitoring, then leave-forward from slide 1
// to 2 triggers. It never consumes CoverHold, so callers can advance the clock
// and assert the hold that unblocks EdgeStarting.
inline WorkerHarness::WorkerHarness(TriggeredAtSlide1Tag) : WorkerHarness() {
  config.startup_ignore = Duration{0};
  plan.machine.targets = {MakeTarget(1, "t1", "p1")};
  plan.targets = {MakeResolvedTarget(1, 1, "t1", "p1")};
  rebuild();
  presentation.show = MakeShowState(HostStatus::Ok, true, 1);
  Start();
  Step();
  presentation.show.slide = 2;
  Step();
}

// Seeds Live: the cover hold launches the Edge, its window becomes ready, the
// show exits and reports ended, and no return title ever arrives.
inline WorkerHarness::WorkerHarness(LiveAtSlide1Tag)
    : WorkerHarness(TriggeredAtSlide1Tag{}) {
  advance(host.cover.fade + host.cover.hold + Duration{1});
  Step();
  windows.windows = {MakeWindow(1, launcher.pid, "TEF-LIVE-STAGE")};
  Step();
  presentation.show = MakeShowState(HostStatus::Ok, false, 0);
  Step();
}

inline WorkerHarness HTriggeredAtSlide1() {
  return WorkerHarness(TriggeredAtSlide1Tag{});
}

inline WorkerHarness HLiveAtSlide1() {
  return WorkerHarness(LiveAtSlide1Tag{});
}

} // namespace htmlppt::test
