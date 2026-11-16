#include "core/edge_controller.h"

namespace htmlppt {

EdgeController::EdgeController(EdgeLauncher& launcher) : launcher_(launcher) {}

bool EdgeController::Launch(const EdgeLaunchSpec& spec) {
  if (active()) return false;
  process_ = launcher_.Launch(spec);
  return process_ != nullptr && process_->alive();
}

void EdgeController::Kill() {
  if (process_ != nullptr) {
    process_->terminate();
    process_.reset();
  }
}

bool EdgeController::active() const {
  return process_ != nullptr && process_->alive();
}

std::uint32_t EdgeController::pid() const {
  return process_ != nullptr ? process_->pid() : 0;
}

} // namespace htmlppt
