#pragma once

#include "core/window_system.h"

namespace htmlppt {

class CoverOverlay {
 public:
  virtual ~CoverOverlay() = default;

  virtual bool Show() = 0;
  virtual void Hide() = 0;
  virtual bool SetOpacity(double opacity) = 0;
  virtual void ReassertTopMost() = 0;

  // Set by the platform when the OS resumed from sleep / the display topology
  // changed; consumed once by the runtime so it can shift deadlines or re-score
  // windows. Platforms without the signal keep the defaults.
  virtual bool ConsumeResumeEvent() { return false; }
  virtual bool ConsumeDisplayChange(ScreenMetrics* metrics) {
    (void)metrics;
    return false;
  }
};

} // namespace htmlppt