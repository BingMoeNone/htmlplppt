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
