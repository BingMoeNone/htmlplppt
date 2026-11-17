#pragma once

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
  virtual const StateSnapshot& snapshot() const = 0;
};

} // namespace htmlppt
