#include "test_main.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/supervisor_protocol.h"

using htmlppt::Crc32;
using htmlppt::DecodeFrame;
using htmlppt::DecodeHeartbeatPayload;
using htmlppt::DecodeStatus;
using htmlppt::Duration;
using htmlppt::EncodeFrame;
using htmlppt::EncodeHeartbeatPayload;
using htmlppt::FakeClock;
using htmlppt::Frame;
using htmlppt::FrameType;
using htmlppt::HeartbeatPayload;
using htmlppt::kProgressNotStarted;
using htmlppt::ProgressTracker;
using htmlppt::SequenceTracker;

namespace {

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

} // namespace

HTMLPPT_TEST_CASE(crc32_known_vector) {
  const auto data = Bytes("123456789");
  HTMLPPT_CHECK_EQ(Crc32(data), std::uint32_t{0xCBF43926u});
}

HTMLPPT_TEST_CASE(frame_round_trip) {
  Frame frame;
  frame.header.sequence = 7;
  frame.header.timestamp_ms = 123456;
  frame.header.type = FrameType::Status;
  frame.payload = Bytes("ready");
  const auto encoded = EncodeFrame(frame);
  HTMLPPT_CHECK_EQ(encoded.size(), htmlppt::kFrameHeaderSize + 5);
  const auto decoded = DecodeFrame(encoded);
  HTMLPPT_CHECK(decoded.status == DecodeStatus::Ok);
  HTMLPPT_CHECK_EQ(decoded.consumed, encoded.size());
  HTMLPPT_CHECK_EQ(decoded.frame.header.sequence, std::uint32_t{7});
  HTMLPPT_CHECK_EQ(decoded.frame.header.timestamp_ms, std::uint64_t{123456});
  HTMLPPT_CHECK(decoded.frame.header.type == FrameType::Status);
  HTMLPPT_CHECK_EQ(decoded.frame.payload, Bytes("ready"));
}

HTMLPPT_TEST_CASE(decode_rejects_bad_magic) {
  auto encoded = EncodeFrame(Frame{});
  encoded[0] = 0;
  const auto decoded = DecodeFrame(encoded);
  HTMLPPT_CHECK(decoded.status == DecodeStatus::BadMagic);
  HTMLPPT_CHECK_EQ(decoded.consumed, std::size_t{1});
}

HTMLPPT_TEST_CASE(decode_rejects_bad_version) {
  auto encoded = EncodeFrame(Frame{});
  encoded[4] = 2;
  const auto decoded = DecodeFrame(encoded);
  HTMLPPT_CHECK(decoded.status == DecodeStatus::BadVersion);
  HTMLPPT_CHECK_EQ(decoded.consumed, htmlppt::kFrameHeaderSize);
}

HTMLPPT_TEST_CASE(decode_rejects_bad_crc) {
  auto encoded = EncodeFrame(Frame{});
  encoded[htmlppt::kFrameHeaderSize - 1] ^= 0xFF;
  const auto decoded = DecodeFrame(encoded);
  HTMLPPT_CHECK(decoded.status == DecodeStatus::BadCrc);
  HTMLPPT_CHECK_EQ(decoded.consumed, htmlppt::kFrameHeaderSize);
}

HTMLPPT_TEST_CASE(decode_reports_incomplete) {
  const auto encoded = EncodeFrame(Frame{});
  const auto decoded = DecodeFrame(
      std::span<const std::uint8_t>(encoded.data(), encoded.size() - 1));
  HTMLPPT_CHECK(decoded.status == DecodeStatus::Incomplete);
  HTMLPPT_CHECK_EQ(decoded.consumed, std::size_t{0});
}

HTMLPPT_TEST_CASE(decode_rejects_oversized_payload) {
  Frame frame;
  frame.payload.resize(htmlppt::kMaxFramePayload + 1);
  HTMLPPT_CHECK(EncodeFrame(frame).empty());
  // A hand-built length field beyond the limit must be rejected without
  // allocating.
  std::vector<std::uint8_t> malicious(htmlppt::kFrameHeaderSize, 0);
  const std::uint32_t length = htmlppt::kMaxFramePayload + 1;
  malicious[0] = 0x46;
  malicious[1] = 0x54;
  malicious[2] = 0x50;
  malicious[3] = 0x48;
  malicious[4] = 1;
  malicious[22] = static_cast<std::uint8_t>(length & 0xFF);
  malicious[23] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
  malicious[24] = static_cast<std::uint8_t>((length >> 16) & 0xFF);
  malicious[25] = static_cast<std::uint8_t>((length >> 24) & 0xFF);
  const auto decoded = DecodeFrame(malicious);
  HTMLPPT_CHECK(decoded.status == DecodeStatus::TooLarge);
  HTMLPPT_CHECK_EQ(decoded.consumed, htmlppt::kFrameHeaderSize);
}

HTMLPPT_TEST_CASE(heartbeat_payload_round_trip) {
  HeartbeatPayload payload;
  payload.state = 7;
  payload.progress = 0x0102030405060708ull;
  const auto encoded = EncodeHeartbeatPayload(payload);
  HTMLPPT_CHECK_EQ(encoded.size(), htmlppt::kHeartbeatPayloadSize);
  const auto decoded = DecodeHeartbeatPayload(encoded);
  HTMLPPT_CHECK(decoded.has_value());
  HTMLPPT_CHECK_EQ(decoded->state, std::uint8_t{7});
  HTMLPPT_CHECK_EQ(decoded->progress, std::uint64_t{0x0102030405060708ull});
}

HTMLPPT_TEST_CASE(heartbeat_payload_rejects_wrong_size) {
  HTMLPPT_CHECK(!DecodeHeartbeatPayload(
                     std::span<const std::uint8_t>()).has_value());
  const std::vector<std::uint8_t> one_byte{1};
  HTMLPPT_CHECK(!DecodeHeartbeatPayload(one_byte).has_value());
}

HTMLPPT_TEST_CASE(progress_tracker_detects_unchanged_progress) {
  FakeClock clock;
  ProgressTracker tracker(clock.now());
  HTMLPPT_CHECK(!tracker.stalled(clock.now(), Duration{2000}));
  HTMLPPT_CHECK(tracker.observe(0, clock.now()));
  clock.advance(Duration{1999});
  HTMLPPT_CHECK(!tracker.stalled(clock.now(), Duration{2000}));
  HTMLPPT_CHECK(!tracker.observe(0, clock.now()));
  clock.advance(Duration{1});
  HTMLPPT_CHECK(tracker.stalled(clock.now(), Duration{2000}));
}

HTMLPPT_TEST_CASE(progress_tracker_advancing_progress_is_healthy) {
  FakeClock clock;
  ProgressTracker tracker(clock.now());
  tracker.observe(0, clock.now());
  clock.advance(Duration{1500});
  HTMLPPT_CHECK(tracker.observe(1, clock.now()));
  clock.advance(Duration{1500});
  HTMLPPT_CHECK(!tracker.stalled(clock.now(), Duration{2000}));
}

// Startup heartbeats carry the sentinel and no loop progress: they must not
// make the tracker look stalled before the worker's loop even starts.
HTMLPPT_TEST_CASE(progress_tracker_startup_sentinel_is_not_a_stall) {
  FakeClock clock;
  ProgressTracker tracker(clock.now());
  HTMLPPT_CHECK(!tracker.observe(kProgressNotStarted, clock.now()));
  clock.advance(Duration{5000});
  HTMLPPT_CHECK(!tracker.stalled(clock.now(), Duration{2000}));
  HTMLPPT_CHECK(!tracker.has_progress());
}

HTMLPPT_TEST_CASE(sequence_tracker_detects_stall) {
  FakeClock clock;
  SequenceTracker tracker(clock.now());
  tracker.observe(1, clock.now());
  clock.advance(Duration{1999});
  HTMLPPT_CHECK(!tracker.stalled(clock.now(), Duration{2000}));
  clock.advance(Duration{1});
  HTMLPPT_CHECK(tracker.stalled(clock.now(), Duration{2000}));
}

HTMLPPT_TEST_CASE(sequence_tracker_ignores_non_monotonic) {
  FakeClock clock;
  SequenceTracker tracker(clock.now());
  HTMLPPT_CHECK(tracker.observe(5, clock.now()));
  clock.advance(Duration{100});
  HTMLPPT_CHECK(!tracker.observe(5, clock.now()));
  HTMLPPT_CHECK(!tracker.observe(4, clock.now()));
  HTMLPPT_CHECK_EQ(tracker.last_sequence(), std::uint32_t{5});
}
