#pragma once

namespace htmlppt {

enum class State {
  Boot,
  Warming,
  Monitoring,
  CoverIn,
  EdgeStarting,
  ShowExit,
  Live,
  Returning,
  ReturnCoverOut,
  Stopping,
  Faulted,
  SafeMode,
  Exited,
  DegradedNoSupervisor,
};

// Operator-facing state name. --status prints it and the smoke script matches
// it, so the spelling is part of the output contract.
inline const char* StateName(State state) {
  switch (state) {
    case State::Boot: return "Boot";
    case State::Warming: return "Warming";
    case State::Monitoring: return "Monitoring";
    case State::CoverIn: return "CoverIn";
    case State::EdgeStarting: return "EdgeStarting";
    case State::ShowExit: return "ShowExit";
    case State::Live: return "Live";
    case State::Returning: return "Returning";
    case State::ReturnCoverOut: return "ReturnCoverOut";
    case State::Stopping: return "Stopping";
    case State::Faulted: return "Faulted";
    case State::SafeMode: return "SafeMode";
    case State::Exited: return "Exited";
    case State::DegradedNoSupervisor: return "DegradedNoSupervisor";
  }
  return "Unknown";
}

enum class SchedulingProfile {
  Adaptive,
  Aggressive,
  Balanced,
  Safe,
};

enum class PowerSource {
  AC,
  Battery,
};

enum class ThreadTier {
  Supervisor,
  Background,
  Ui,
  Focus,
  Critical,
};

enum class ThreadRole {
  Supervisor,
  IpcHealth,
  UiOverlay,
  Focus,
  ComState,
};

enum class TriggerMode {
  LeaveForward,
  Arrive,
};

enum class OnFailurePolicy {
  SelfHeal,
  Strict,
  Safe,
  Legacy,
};

enum class FailureKind {
  Transient,
  Switch,
  Return,
  Structural,
};

} // namespace htmlppt
