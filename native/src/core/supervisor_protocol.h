#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/clock.h"

namespace htmlppt {

inline constexpr std::uint32_t kFrameMagic = 0x48505446u;  // "HPTF"
// Bumped to 2 with the 10-byte heartbeat payload (state + progress + flags): a
// mixed-build peer must fail the handshake with BadVersion instead of accepting
// a frame whose extra byte the old decoder would silently drop.
inline constexpr std::uint16_t kFrameVersion = 2;
inline constexpr std::size_t kFrameHeaderSize = 30;
inline constexpr std::size_t kMaxFramePayload = 64 * 1024;

enum class FrameType : std::uint16_t {
  Heartbeat = 1,
  Status = 2,
  Log = 3,
  Stop = 4,
  Restart = 5,
  Cleanup = 6,
  // Hot reload handshake: the supervisor asks the worker to stop stepping, waits
  // for a heartbeat carrying kHeartbeatFlagPaused, then rebuilds the plan and
  // replaces the process. Resume lets the worker continue when the rebuild
  // failed. An older worker ignores both frames; the supervisor then times out
  // and leaves the reload request for the next safe point.
  Pause = 7,
  Resume = 8,
};

struct FrameHeader {
  std::uint32_t magic = kFrameMagic;
  std::uint16_t version = kFrameVersion;
  std::uint16_t flags = 0;
  std::uint32_t sequence = 0;
  std::uint64_t timestamp_ms = 0;
  FrameType type = FrameType::Heartbeat;
  std::uint32_t payload_length = 0;
  std::uint32_t crc32 = 0;
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

// Heartbeat payload: one state byte, the worker's loop progress as a
// little-endian uint64, and one flags byte. Progress is a step counter the
// session publishes after each Step() returns, so an advancing sequence with a
// static progress means the loop is wedged in a call even though the helper
// thread still heartbeats. The flags byte carries the loop's real safe point:
// bit 0 is set only while the loop is parked in the bounded Monitoring wait
// between Steps, so the supervisor never swaps the worker mid-transition.
inline constexpr std::size_t kHeartbeatPayloadSize = 10;
inline constexpr std::uint8_t kHeartbeatFlagSafePoint = 1u << 0;
// Set while the session honours a Pause frame and no longer calls Step. Hot
// reload waits for this acknowledgement before rebuilding the plan, so the
// worker cannot advance into a transition between the safe-point heartbeat and
// the termination.
inline constexpr std::uint8_t kHeartbeatFlagPaused = 1u << 1;
// Startup heartbeats run before the session's loop exists; they publish this
// sentinel so the supervisor does not mistake startup for a stalled loop.
inline constexpr std::uint64_t kProgressNotStarted = ~std::uint64_t{0};

struct HeartbeatPayload {
  std::uint8_t state = 0;
  std::uint64_t progress = kProgressNotStarted;
  std::uint8_t flags = 0;
};

std::vector<std::uint8_t> EncodeHeartbeatPayload(
    const HeartbeatPayload& payload);
// Returns nullopt for a payload that is not exactly kHeartbeatPayloadSize
// bytes, such as a frame written by a worker without loop progress.
std::optional<HeartbeatPayload> DecodeHeartbeatPayload(
    std::span<const std::uint8_t> payload);

std::uint32_t Crc32(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> EncodeFrame(const Frame& frame);

enum class DecodeStatus {
  Ok,
  Incomplete,
  BadMagic,
  BadVersion,
  TooLarge,
  BadCrc,
};

struct DecodeResult {
  DecodeStatus status = DecodeStatus::Incomplete;
  Frame frame;
  std::size_t consumed = 0;
};

DecodeResult DecodeFrame(std::span<const std::uint8_t> data);

class SequenceTracker {
 public:
  explicit SequenceTracker(ClockPoint start);

  bool observe(std::uint32_t sequence, ClockPoint now);
  bool stalled(ClockPoint now, Duration timeout) const;
  std::uint32_t last_sequence() const;
  void reset(ClockPoint start);

 private:
  ClockPoint start_;
  std::optional<std::uint32_t> last_sequence_;
  ClockPoint last_update_{};
};

// Tracks the worker loop's published progress. It is intentionally separate
// from SequenceTracker: the heartbeat helper thread keeps the sequence moving
// while the loop itself is stuck, so only progress can reveal a hang.
class ProgressTracker {
 public:
  explicit ProgressTracker(ClockPoint start);

  // Records a heartbeat's progress. Returns true when the value advanced.
  // kProgressNotStarted clears the sample (startup); a repeated or older value
  // is not an advance and leaves the stall reference unchanged.
  bool observe(std::uint64_t progress, ClockPoint now);
  // True once a real progress sample has remained unchanged for `timeout`.
  bool stalled(ClockPoint now, Duration timeout) const;
  bool has_progress() const { return last_progress_.has_value(); }
  std::uint64_t last_progress() const { return last_progress_.value_or(0); }
  void reset(ClockPoint start);

 private:
  std::optional<std::uint64_t> last_progress_;
  ClockPoint last_update_{};
};

} // namespace htmlppt
