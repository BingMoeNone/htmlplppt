#include "core/supervisor_protocol.h"

#include <algorithm>

namespace htmlppt {
namespace {

void WriteU16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFF);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void WriteU32(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value & 0xFF);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
}

void WriteU64(std::uint8_t* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
  }
}

std::uint16_t ReadU16(const std::uint8_t* in) {
  return static_cast<std::uint16_t>(in[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8);
}

std::uint32_t ReadU32(const std::uint8_t* in) {
  return static_cast<std::uint32_t>(in[0]) |
         (static_cast<std::uint32_t>(in[1]) << 8) |
         (static_cast<std::uint32_t>(in[2]) << 16) |
         (static_cast<std::uint32_t>(in[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

} // namespace

std::vector<std::uint8_t> EncodeHeartbeatPayload(
    const HeartbeatPayload& payload) {
  std::vector<std::uint8_t> out(kHeartbeatPayloadSize, 0);
  out[0] = payload.state;
  WriteU64(out.data() + 1, payload.progress);
  return out;
}

std::optional<HeartbeatPayload> DecodeHeartbeatPayload(
    std::span<const std::uint8_t> payload) {
  if (payload.size() != kHeartbeatPayloadSize) return std::nullopt;
  HeartbeatPayload decoded;
  decoded.state = payload[0];
  decoded.progress = ReadU64(payload.data() + 1);
  return decoded;
}

std::uint32_t Crc32(std::span<const std::uint8_t> data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::uint8_t byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return ~crc;
}

std::vector<std::uint8_t> EncodeFrame(const Frame& frame) {
  if (frame.payload.size() > kMaxFramePayload) return {};
  std::vector<std::uint8_t> out(kFrameHeaderSize + frame.payload.size(), 0);
  WriteU32(out.data(), kFrameMagic);
  WriteU16(out.data() + 4, frame.header.version);
  WriteU16(out.data() + 6, frame.header.flags);
  WriteU32(out.data() + 8, frame.header.sequence);
  WriteU64(out.data() + 12, frame.header.timestamp_ms);
  WriteU16(out.data() + 20, static_cast<std::uint16_t>(frame.header.type));
  WriteU32(out.data() + 22,
           static_cast<std::uint32_t>(frame.payload.size()));
  if (!frame.payload.empty()) {
    std::copy(frame.payload.begin(), frame.payload.end(),
              out.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize));
  }
  const auto crc = Crc32(out);
  WriteU32(out.data() + 26, crc);
  return out;
}

DecodeResult DecodeFrame(std::span<const std::uint8_t> data) {
  DecodeResult result;
  if (data.size() < kFrameHeaderSize) return result;
  if (ReadU32(data.data()) != kFrameMagic) {
    result.status = DecodeStatus::BadMagic;
    result.consumed = 1;
    return result;
  }
  if (ReadU16(data.data() + 4) != kFrameVersion) {
    result.status = DecodeStatus::BadVersion;
    result.consumed = kFrameHeaderSize;
    return result;
  }
  const auto payload_length = ReadU32(data.data() + 22);
  if (payload_length > kMaxFramePayload) {
    result.status = DecodeStatus::TooLarge;
    result.consumed = kFrameHeaderSize;
    return result;
  }
  const std::size_t total = kFrameHeaderSize + payload_length;
  if (data.size() < total) return result;
  std::vector<std::uint8_t> received(data.begin(), data.begin() + total);
  const auto expected = ReadU32(received.data() + 26);
  WriteU32(received.data() + 26, 0);
  if (Crc32(received) != expected) {
    result.status = DecodeStatus::BadCrc;
    result.consumed = total;
    return result;
  }
  result.status = DecodeStatus::Ok;
  result.consumed = total;
  result.frame.header.magic = kFrameMagic;
  result.frame.header.version = kFrameVersion;
  result.frame.header.flags = ReadU16(data.data() + 6);
  result.frame.header.sequence = ReadU32(data.data() + 8);
  result.frame.header.timestamp_ms = ReadU64(data.data() + 12);
  result.frame.header.type =
      static_cast<FrameType>(ReadU16(data.data() + 20));
  result.frame.header.payload_length = payload_length;
  result.frame.header.crc32 = expected;
  result.frame.payload.assign(
      data.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize),
      data.begin() + static_cast<std::ptrdiff_t>(total));
  return result;
}

SequenceTracker::SequenceTracker(ClockPoint start)
    : start_(start), last_update_(start) {}

bool SequenceTracker::observe(std::uint32_t sequence, ClockPoint now) {
  if (last_sequence_.has_value() && sequence <= *last_sequence_) {
    return false;
  }
  last_sequence_ = sequence;
  last_update_ = now;
  return true;
}

bool SequenceTracker::stalled(ClockPoint now, Duration timeout) const {
  const ClockPoint reference =
      last_sequence_.has_value() ? last_update_ : start_;
  return now - reference >= timeout;
}

std::uint32_t SequenceTracker::last_sequence() const {
  return last_sequence_.value_or(0);
}

void SequenceTracker::reset(ClockPoint start) {
  start_ = start;
  last_sequence_.reset();
  last_update_ = start;
}

ProgressTracker::ProgressTracker(ClockPoint start) : last_update_(start) {}

bool ProgressTracker::observe(std::uint64_t progress, ClockPoint now) {
  if (progress == kProgressNotStarted) {
    // A startup heartbeat carries no loop progress. Clear any sample so the
    // count starts at the session's first real value, not at process start.
    last_progress_.reset();
    return false;
  }
  if (last_progress_.has_value() && progress <= *last_progress_) return false;
  last_progress_ = progress;
  last_update_ = now;
  return true;
}

bool ProgressTracker::stalled(ClockPoint now, Duration timeout) const {
  // Before the first loop heartbeat there is no progress to judge; the
  // sequence tracker still covers a worker that stops heartbeating entirely.
  if (!last_progress_.has_value()) return false;
  return now - last_update_ >= timeout;
}

void ProgressTracker::reset(ClockPoint start) {
  last_progress_.reset();
  last_update_ = start;
}

} // namespace htmlppt
