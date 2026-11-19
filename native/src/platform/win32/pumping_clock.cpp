#include "platform/win32/pumping_clock.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace htmlppt {

ClockPoint PumpingClock::now() const {
  return std::chrono::steady_clock::now();
}

void PumpingClock::sleep_until(ClockPoint deadline) {
  for (;;) {
    const auto current = std::chrono::steady_clock::now();
    if (current >= deadline) return;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - current)
                         .count();
    if (remaining < 1) remaining = 1;
    const auto slice =
        static_cast<DWORD>(std::min<long long>(remaining, kMaxSlice.count()));
    // MWMO_INPUTAVAILABLE: a message already in the queue must wake us, not only
    // one that arrives after the call.
    MsgWaitForMultipleObjectsEx(0, nullptr, slice, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
}

} // namespace htmlppt
