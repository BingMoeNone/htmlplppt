#include "core/failure_policy.h"

namespace htmlppt {

FailureKind EscalateFailure(State state) {
  switch (state) {
    case State::CoverIn:
    case State::EdgeStarting:
    case State::ShowExit:
      return FailureKind::Switch;
    case State::Live:
    case State::Returning:
    case State::ReturnCoverOut:
      return FailureKind::Return;
    default:
      return FailureKind::Structural;
  }
}

ComErrorWindow::ComErrorWindow(Duration window) : window_(window) {}

void ComErrorWindow::record(ClockPoint now) {
  if (!first_error_.has_value()) first_error_ = now;
}

void ComErrorWindow::reset() { first_error_.reset(); }

bool ComErrorWindow::expired(ClockPoint now) const {
  return first_error_.has_value() && now - *first_error_ >= window_;
}

bool ComErrorWindow::active() const { return first_error_.has_value(); }

} // namespace htmlppt
