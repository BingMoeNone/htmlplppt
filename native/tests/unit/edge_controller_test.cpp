#include "test_main.h"

#include <cstdint>
#include <memory>
#include <utility>

#include "core/edge_controller.h"

using htmlppt::EdgeController;
using htmlppt::EdgeLaunchSpec;
using htmlppt::EdgeLauncher;
using htmlppt::EdgeProcess;

namespace {

struct FakeState {
  bool alive = true;
  bool terminated = false;
  bool released = false;
};

class FakeEdgeProcess final : public EdgeProcess {
 public:
  FakeEdgeProcess(std::uint32_t pid, std::shared_ptr<FakeState> state)
      : pid_(pid), state_(std::move(state)) {}
  std::uint32_t pid() const override { return pid_; }
  bool alive() const override { return state_->alive; }
  void terminate() override {
    state_->alive = false;
    state_->terminated = true;
  }
  bool release() override {
    state_->released = true;
    return true;
  }

 private:
  std::uint32_t pid_ = 0;
  std::shared_ptr<FakeState> state_;
};

class FakeLauncher final : public EdgeLauncher {
 public:
  std::unique_ptr<EdgeProcess> Launch(const EdgeLaunchSpec&) override {
    ++launch_count;
    if (!succeed) return nullptr;
    last_state = std::make_shared<FakeState>();
    return std::make_unique<FakeEdgeProcess>(next_pid++, last_state);
  }
  int launch_count = 0;
  std::uint32_t next_pid = 100;
  bool succeed = true;
  std::shared_ptr<FakeState> last_state;
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
  HTMLPPT_CHECK(launcher.last_state->terminated);
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

HTMLPPT_TEST_CASE(edge_controller_destructor_terminates_active_process) {
  FakeLauncher launcher;
  {
    EdgeController controller(launcher);
    HTMLPPT_CHECK(controller.Launch(EdgeLaunchSpec{}));
  }
  HTMLPPT_CHECK(launcher.last_state->terminated);
  HTMLPPT_CHECK(!launcher.last_state->alive);
}

HTMLPPT_TEST_CASE(edge_controller_release_detaches_without_terminating) {
  FakeLauncher launcher;
  {
    EdgeController controller(launcher);
    HTMLPPT_CHECK(controller.Launch(EdgeLaunchSpec{}));
    const auto state = launcher.last_state;
    HTMLPPT_CHECK(controller.Release());
    HTMLPPT_CHECK(state->released);
    HTMLPPT_CHECK(!state->terminated);
    HTMLPPT_CHECK(!controller.active());
    HTMLPPT_CHECK_EQ(controller.pid(), std::uint32_t{0});
  }
  HTMLPPT_CHECK(!launcher.last_state->terminated);
}
