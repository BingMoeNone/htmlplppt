#include "core/cover_timeline.h"

#include <algorithm>

namespace htmlppt {
namespace {

double Ratio(ClockPoint now, ClockPoint start, Duration duration) {
  if (duration.count() <= 0) return 1.0;
  const auto elapsed = std::chrono::duration_cast<Duration>(now - start).count();
  if (elapsed <= 0) return 0.0;
  return std::min(1.0, static_cast<double>(elapsed) /
                           static_cast<double>(duration.count()));
}

} // namespace

CoverTimeline::CoverTimeline(CoverTiming timing) : timing_(timing) {}

void CoverTimeline::FadeIn(ClockPoint now) {
  phase_ = CoverPhase::FadeIn;
  phase_start_ = now;
  opacity_ = 0.0;
  fade_out_pending_ = false;
}

void CoverTimeline::FadeOut(ClockPoint now) {
  (void)now;
  if (phase_ == CoverPhase::Idle) return;
  fade_out_pending_ = true;
}

void CoverTimeline::Hide() {
  phase_ = CoverPhase::Idle;
  opacity_ = 0.0;
  fade_out_pending_ = false;
}

CoverTick CoverTimeline::Tick(ClockPoint now) {
  CoverTick tick;
  tick.opacity = opacity_;

  switch (phase_) {
    case CoverPhase::Idle:
      tick.opacity = 0.0;
      return tick;
    case CoverPhase::FadeIn: {
      tick.opacity = Ratio(now, phase_start_, timing_.fade);
      opacity_ = tick.opacity;
      if (tick.opacity >= 1.0) {
        phase_ = CoverPhase::Hold;
        hold_start_ = now;
        opacity_ = 1.0;
        tick.opacity = 1.0;
        tick.hold_reached = true;
      }
      return tick;
    }
    case CoverPhase::Hold: {
      tick.opacity = 1.0;
      const auto held =
          std::chrono::duration_cast<Duration>(now - hold_start_).count();
      const bool hold_done = held >= timing_.hold.count();
      const bool max_done = held >= timing_.max_hold.count();
      if ((fade_out_pending_ && hold_done) || max_done) {
        phase_ = CoverPhase::FadeOut;
        phase_start_ = now;
        fade_out_pending_ = false;
      }
      return tick;
    }
    case CoverPhase::FadeOut: {
      const double ratio = Ratio(now, phase_start_, timing_.fade);
      tick.opacity = 1.0 - ratio;
      opacity_ = tick.opacity;
      if (ratio >= 1.0) {
        phase_ = CoverPhase::Idle;
        opacity_ = 0.0;
        tick.opacity = 0.0;
        tick.done = true;
      }
      return tick;
    }
  }
  return tick;
}

std::optional<ClockPoint> CoverTimeline::NextDeadline() const {
  switch (phase_) {
    case CoverPhase::Idle:
      return std::nullopt;
    case CoverPhase::FadeIn:
      return phase_start_ + timing_.fade;
    case CoverPhase::Hold: {
      const auto max_deadline = hold_start_ + timing_.max_hold;
      if (fade_out_pending_) {
        return std::min(hold_start_ + timing_.hold, max_deadline);
      }
      return max_deadline;
    }
    case CoverPhase::FadeOut:
      return phase_start_ + timing_.fade;
  }
  return std::nullopt;
}

} // namespace htmlppt
