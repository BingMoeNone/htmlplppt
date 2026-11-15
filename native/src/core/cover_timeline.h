#pragma once

#include <optional>

#include "core/clock.h"

namespace htmlppt {

enum class CoverPhase {
  Idle,
  FadeIn,
  Hold,
  FadeOut,
};

struct CoverTiming {
  Duration fade{300};
  Duration hold{1000};
  Duration max_hold{10000};
};

struct CoverTick {
  double opacity = 0.0;
  bool hold_reached = false;
  bool done = false;
};

class CoverTimeline {
 public:
  explicit CoverTimeline(CoverTiming timing);

  void FadeIn(ClockPoint now);
  void FadeOut(ClockPoint now);
  void Hide();

  CoverTick Tick(ClockPoint now);

  CoverPhase phase() const { return phase_; }
  double opacity() const { return opacity_; }
  std::optional<ClockPoint> NextDeadline() const;

 private:
  double OpacityFor(ClockPoint now, Duration duration) const;

  CoverTiming timing_;
  CoverPhase phase_ = CoverPhase::Idle;
  ClockPoint phase_start_{};
  ClockPoint hold_start_{};
  double opacity_ = 0.0;
  bool fade_out_pending_ = false;
};

} // namespace htmlppt
