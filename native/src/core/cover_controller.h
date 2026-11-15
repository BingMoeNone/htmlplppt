#pragma once

#include "core/clock.h"
#include "core/cover_overlay.h"
#include "core/cover_timeline.h"

namespace htmlppt {

class CoverController {
 public:
  CoverController(CoverTiming timing, Clock& clock, CoverOverlay& overlay);

  bool FadeIn();
  void FadeOut();
  void Hide();
  CoverTick Tick();

 private:
  CoverTimeline timeline_;
  Clock& clock_;
  CoverOverlay& overlay_;
  bool visible_ = false;
};

} // namespace htmlppt
