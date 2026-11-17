#pragma once

#ifdef _WIN32

#include "core/clock.h"

namespace htmlppt {

// Clock for the worker loop. Its wait pumps the thread's message queue, so STA
// COM callbacks and the cover window's messages are still delivered during an
// idle sleep; std::this_thread::sleep_until would starve them.
//
// This is deliberately not a threading rework: the loop stays single-threaded
// and only changes how it waits.
class PumpingClock final : public Clock {
 public:
  ClockPoint now() const override;
  void sleep_until(ClockPoint deadline) override;

  // Longest single wait. Shorter slices keep the queue responsive while still
  // honouring the deadline.
  static constexpr Duration kMaxSlice{50};
};

} // namespace htmlppt

#endif // _WIN32
