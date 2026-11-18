#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/clock.h"
#include "core/state_machine_types.h"
#include "core/worker_driver.h"

namespace htmlppt::test {

// Records every session call and plays a scripted lifetime: it either finishes
// after a fixed number of steps or as soon as a stop request arrives. The
// session tests use it so the pipe, heartbeat, and supervisor-loss behavior can
// be checked without WPS or Edge.
class FakeWorkerDriver final : public WorkerDriver {
 public:
  void Start() override { ++start_calls; }

  bool Step() override {
    ++step_calls;
    // The first step can block longer than the rest, which is how a real COM
    // action behaves; the heartbeat progress tests use it to hold the loop
    // inside Step while the helper thread keeps sending. Without an override
    // every step uses step_sleep.
    const Duration delay =
        step_calls == 1 && first_step_sleep.count() > 0 ? first_step_sleep
                                                       : step_sleep;
    if (delay.count() > 0) {
      std::this_thread::sleep_for(delay);
    }
    if (step_calls <= static_cast<int>(scripted_states.size())) {
      state_snapshot.state =
          scripted_states[static_cast<std::size_t>(step_calls - 1)];
    }
    if (steps_to_done > 0 && step_calls >= steps_to_done) done_ = true;
    return !done_;
  }

  bool done() const override { return done_; }

  void RequestStop(std::string reason) override {
    ++request_stop_calls;
    stop_reasons.push_back(std::move(reason));
    if (stop_ends) done_ = true;
  }

  void HandleSupervisorLost(std::string detail) override {
    ++lost_calls;
    lost_details.push_back(std::move(detail));
    step_calls_at_loss = step_calls;
  }

  const StateSnapshot& snapshot() const override { return state_snapshot; }

  StateSnapshot state_snapshot;
  // One state per Step, applied before that Step returns. Empty keeps the state
  // the test set on state_snapshot. The session's on_step callback tests script
  // real transitions with it.
  std::vector<State> scripted_states;
  int start_calls = 0;
  int step_calls = 0;
  // 0 means only a stop request finishes the run.
  int steps_to_done = 0;
  bool stop_ends = true;
  Duration step_sleep{0};
  // Extra delay for the first Step only; 0 uses step_sleep for every step.
  Duration first_step_sleep{0};
  int request_stop_calls = 0;
  std::vector<std::string> stop_reasons;
  int lost_calls = 0;
  std::vector<std::string> lost_details;
  // The step count when HandleSupervisorLost was called, or -1.
  int step_calls_at_loss = -1;

 private:
  bool done_ = false;
};

} // namespace htmlppt::test
