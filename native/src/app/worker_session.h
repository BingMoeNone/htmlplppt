#pragma once

#ifdef _WIN32

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/clock.h"
#include "core/worker_driver.h"
#include "core/worker_plan.h"

namespace htmlppt {

class FrameChannel;
class SharedBlackbox;

struct WorkerSessionConfig {
  // One heartbeat frame per interval, carrying snapshot().state plus the loop
  // progress counter. 200 ms is the design-spec value (spec 11.2 heartbeat
  // table), ten times inside the supervisor's 2 s heartbeat timeout.
  Duration heartbeat_interval{200};
  // A blackbox record on the first heartbeat and then every N heartbeats.
  int blackbox_every = 5;
  // The last sequence a startup phase already sent, so heartbeat numbers stay
  // strictly increasing across startup and the session.
  std::uint32_t initial_sequence = 0;
  // Called on the heartbeat thread after each successful send. It must not
  // block or touch COM: the worker uses it to refresh the legacy watcher.lock
  // so VBS-era tooling still sees a live run.
  std::function<void()> on_heartbeat;
  // Polled once per loop iteration; true means an external stop is requested
  // (spec 11.3: the legacy stop-live.vbs deletes watcher.lock). It must be
  // cheap -- the session throttles by calling it at the loop cadence only.
  std::function<bool()> external_stop;
  // Spec 6.6 fault injection: tell the driver the supervisor is gone right after
  // start, without breaking the pipe. The loop must keep running.
  bool inject_supervisor_loss = false;
};

// Rejects a plan written for a different run, such as a stale worker-plan.json
// left behind by a failed plan write in an earlier run. The message names both
// ids; *error is cleared on success.
bool ValidateWorkerPlanRunId(const WorkerPlan& plan, std::string_view run_id,
                             std::string* error);

// Sends heartbeats while `startup` runs on the calling thread. The caller keeps
// its COM apartment; a helper thread only writes frames and blackbox records.
// This keeps the supervisor's heartbeat window filled while a slow adapter
// start (WPS COM activation can take seconds) blocks the caller. Returns the
// last sequence sent and reports through *pipe_alive whether the channel held.
std::uint32_t RunWithStartupHeartbeats(FrameChannel& channel,
                                       SharedBlackbox& blackbox,
                                       WorkerSessionConfig config,
                                       const std::function<void()>& startup,
                                       bool* pipe_alive);

// Runs the session: starts the driver, polls the supervisor pipe with a zero
// timeout, feeds Stop frames to WorkerDriver::RequestStop, and sends a
// heartbeat every interval on a helper thread so a blocking Step cannot starve
// the supervisor. The heartbeat carries the last state the loop published (one
// step stale at worst) plus a step counter incremented after each Step()
// returns; an advancing sequence with a static progress is how the supervisor
// detects a loop wedged in a call (a hung worker still heartbeats). It is the
// only writer of the blackbox during the loop.
// A broken pipe is reported once through WorkerDriver::HandleSupervisorLost and
// does not stop the loop (SupervisorLossPolicy::Continue). Stops the heartbeat
// thread, then sends a final Cleanup frame while the pipe is alive, and returns
// the exit code. *error names a supervisor loss or an undelivered Cleanup
// frame; it is diagnostic only and never changes the code.
int RunWorkerSession(WorkerDriver& driver, FrameChannel& channel,
                     SharedBlackbox& blackbox, WorkerSessionConfig config,
                     std::string* error);

} // namespace htmlppt

#endif // _WIN32
