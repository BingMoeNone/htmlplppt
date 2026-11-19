#pragma once

#include <atomic>
#include <string>

#include "core/state_machine_types.h"

namespace htmlppt {

// The loop-facing view of a worker process. WorkerSession drives this interface
// and WorkerRuntime implements it; the seam lets the session's pipe, heartbeat,
// stop, and supervisor-loss behavior be tested without WPS or Edge.
class WorkerDriver {
 public:
  virtual ~WorkerDriver() = default;

  virtual void Start() = 0;
  // Advances one bounded poll. Returns false once the worker has finished.
  virtual bool Step() = 0;
  virtual bool done() const = 0;
  virtual void RequestStop(std::string reason) = 0;
  virtual void HandleSupervisorLost(std::string detail) = 0;
  // Installs the session's heartbeat safe-point flag. The driver stores true only
  // while it is parked in Monitoring between Steps, so the session's heartbeat
  // thread can advertise the one instant a hot reload may replace the worker;
  // every other moment (mid startup, switch/return, or stop) must read false.
  // The pointer must stay valid until the session clears it with nullptr.
  virtual void SetSafePointFlag(std::atomic<bool>* flag) = 0;
  virtual const StateSnapshot& snapshot() const = 0;
};

} // namespace htmlppt
