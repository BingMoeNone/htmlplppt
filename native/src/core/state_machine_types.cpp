#include "core/state_machine_types.h"

#include <utility>

namespace htmlppt {

Event MakeSlideChanged(ClockPoint at, int old_slide, int new_slide) {
  Event event;
  event.kind = EventKind::SlideChanged;
  event.at = at;
  event.old_slide = old_slide;
  event.new_slide = new_slide;
  return event;
}

Event MakeShowStarted(ClockPoint at) {
  Event event;
  event.kind = EventKind::ShowStarted;
  event.at = at;
  return event;
}

Event MakeShowEnded(ClockPoint at) {
  Event event;
  event.kind = EventKind::ShowEnded;
  event.at = at;
  return event;
}

Event MakeCoverHold(ClockPoint at) {
  Event event;
  event.kind = EventKind::CoverHold;
  event.at = at;
  return event;
}

Event MakeCoverDone(ClockPoint at) {
  Event event;
  event.kind = EventKind::CoverDone;
  event.at = at;
  return event;
}

Event MakeEdgeReady(ClockPoint at) {
  Event event;
  event.kind = EventKind::EdgeReady;
  event.at = at;
  return event;
}

Event MakeEdgeClosed(ClockPoint at) {
  Event event;
  event.kind = EventKind::EdgeClosed;
  event.at = at;
  return event;
}

Event MakeReturnRequested(ClockPoint at) {
  Event event;
  event.kind = EventKind::ReturnRequested;
  event.at = at;
  return event;
}

Event MakeTimeout(ClockPoint at, TimeoutKind kind) {
  Event event;
  event.kind = EventKind::Timeout;
  event.at = at;
  event.timeout = kind;
  return event;
}

Event MakeStopRequested(ClockPoint at, std::string source) {
  Event event;
  event.kind = EventKind::StopRequested;
  event.at = at;
  event.detail = std::move(source);
  return event;
}

Event MakeCleanupDone(ClockPoint at) {
  Event event;
  event.kind = EventKind::CleanupDone;
  event.at = at;
  return event;
}

Event MakePowerResume(ClockPoint at) {
  Event event;
  event.kind = EventKind::PowerResume;
  event.at = at;
  return event;
}

Event MakeFailure(ClockPoint at, FailureKind kind, FailureSource source,
                  std::string detail) {
  Event event;
  event.kind = EventKind::Failure;
  event.at = at;
  event.failure = kind;
  event.failure_source = source;
  event.detail = std::move(detail);
  return event;
}

Event MakeSupervisorLost(ClockPoint at, std::string detail) {
  Event event;
  event.kind = EventKind::SupervisorLost;
  event.at = at;
  event.detail = std::move(detail);
  return event;
}

} // namespace htmlppt
