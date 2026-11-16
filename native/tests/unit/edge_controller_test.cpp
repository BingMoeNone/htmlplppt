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
