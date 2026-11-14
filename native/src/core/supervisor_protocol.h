#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/clock.h"

namespace htmlppt {

inline constexpr std::uint32_t kFrameMagic = 0x48505446u;  // "HPTF"
inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 30;
inline constexpr std::size_t kMaxFramePayload = 64 * 1024;

enum class FrameType : std::uint16_t {
  Heartbeat = 1,
  Status = 2,
  Log = 3,
  Stop = 4,
  Restart = 5,
  Cleanup = 6,
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

} // namespace htmlppt
