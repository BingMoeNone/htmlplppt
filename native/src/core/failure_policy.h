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
