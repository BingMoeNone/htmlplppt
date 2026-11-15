#include "core/cover_controller.h"

namespace htmlppt {

CoverController::CoverController(CoverTiming timing, Clock& clock,
                                 CoverOverlay& overlay)
    : timeline_(timing), clock_(clock), overlay_(overlay) {}

bool CoverController::FadeIn() {
  timeline_.FadeIn(clock_.now());
  visible_ = overlay_.Show();
  if (visible_) overlay_.SetOpacity(0.0);
  return visible_;
}

void CoverController::FadeOut() {
  timeline_.FadeOut(clock_.now());
}

void CoverController::Hide() {
  timeline_.Hide();
  overlay_.Hide();
  visible_ = false;
}

CoverTick CoverController::Tick() {
  CoverTick tick = timeline_.Tick(clock_.now());
  if (visible_) {
    overlay_.SetOpacity(tick.opacity);
    if (tick.hold_reached) overlay_.ReassertTopMost();
    if (tick.done) {
      overlay_.Hide();
      visible_ = false;
    }
  }
  return tick;
}

} // namespace htmlppt
