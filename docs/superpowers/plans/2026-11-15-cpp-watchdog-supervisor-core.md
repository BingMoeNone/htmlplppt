# C++ Watchdog Supervisor Core Implementation Plan (Plan 4A)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the platform-neutral supervisor core: IPC frame encoding/CRC, sequence-stall detection, black-box ring buffer, worker thread health monitoring, restart/backoff policy, supervisor decision engine, single-instance identity, and startup journal reconciliation.

**Architecture:** `core/supervisor_protocol` owns the wire format and sequence tracking. `core/blackbox` owns the fixed-record ring buffer. `core/worker_health` owns per-thread heartbeats. `core/supervisor` consumes worker events and emits declarative supervisor actions with restart budgets and backoff. `core/single_instance` and `core/journal_reconcile` own stable instance identity, lock-line compatibility, and startup reconciliation decisions. No Win32, COM, named pipes, shared memory, or MiniDump code is introduced; Plan 4B maps these cores onto Windows.

**Tech Stack:** C++20, CMake/CTest, the Plan 1 `Clock`/`RetryPolicy` primitives, nlohmann/json for lock-line and journal-adjacent parsing, and the existing zero-dependency test harness.

## Global Constraints

- Target Windows 10/11 x64; Plan 4A must build and test on Linux/WSL.
- C++20; MSVC `/W4 /permissive- /utf-8`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code lives under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only. Configuration and log content is UTF-8 at runtime.
- `core/` must not include Windows headers. No COM, Win32, named pipe, shared-memory, or MiniDump calls in this plan.
- Monotonic time only. Use `ClockPoint`/`Duration` and the injected `Clock`; no wall-clock time.
- The supervisor core does not parse business configuration and does not call COM.
- Defaults: heartbeat timeout 2s, restart budget 5 restarts per 10 minutes, backoff 200ms/500ms/1s/2s/5s with jitter, black-box record size 96 bytes, frame payload limit 64 KiB.
- Existing suite (145 unit cases, CTest 8/8) must stay green.
- Final gate expected: 184/184 unit cases, CTest 8/8, `npm test` 3/3 with `CHROME_BIN` in this WSL environment.

## File Structure

Create:

- `native/src/core/supervisor_protocol.h` - frame types, CRC32, encode/decode, sequence tracker.
- `native/src/core/supervisor_protocol.cpp` - little-endian wire encoding, CRC32, stream decoding.
- `native/src/core/blackbox.h` - fixed-record black-box ring buffer.
- `native/src/core/blackbox.cpp` - append/snapshot/overwrite semantics.
- `native/src/core/worker_health.h` - per-thread heartbeat registry.
- `native/src/core/worker_health.cpp` - register/beat/stale checks.
- `native/src/core/supervisor.h` - supervisor config, events, actions, decision engine.
- `native/src/core/supervisor.cpp` - heartbeat monitoring, restart budget, backoff, safe mode.
- `native/src/core/single_instance.h` - stable instance fingerprint, lock-line format/parse.
- `native/src/core/single_instance.cpp` - FNV-1a identity, JSON lock line, legacy detection.
- `native/src/core/journal_reconcile.h` - startup reconciliation facts and decisions.
- `native/src/core/journal_reconcile.cpp` - deterministic reconciliation rules.
- `native/tests/unit/supervisor_protocol_test.cpp` - protocol tests.
- `native/tests/unit/blackbox_test.cpp` - ring-buffer tests.
- `native/tests/unit/worker_health_test.cpp` - heartbeat tests.
- `native/tests/unit/supervisor_test.cpp` - decision-engine tests.
- `native/tests/unit/single_instance_test.cpp` - identity/lock tests.
- `native/tests/unit/journal_reconcile_test.cpp` - reconciliation tests.

Modify:

- `native/README.md` - document the supervisor-core contract and the Plan 4B platform mapping.
- `docs/superpowers/plans/2026-11-15-cpp-watchdog-supervisor-core.md` - append as-built amendments after execution.

---

### Task 1: Supervisor Protocol and Sequence Tracking

**Files:**
- Create: `native/src/core/supervisor_protocol.h`
- Create: `native/src/core/supervisor_protocol.cpp`
- Create: `native/tests/unit/supervisor_protocol_test.cpp`

**Interfaces:**
- Consumes: `ClockPoint`, `Duration` from `core/clock.h`.
- Produces:
  - `constexpr std::uint32_t kFrameMagic = 0x48505446u` ("HPTF")
  - `constexpr std::uint16_t kFrameVersion = 1`
  - `constexpr std::size_t kFrameHeaderSize = 30`
  - `constexpr std::size_t kMaxFramePayload = 64 * 1024`
  - `enum class FrameType : std::uint16_t { Heartbeat=1, Status=2, Log=3, Stop=4, Restart=5, Cleanup=6 }`
  - `struct FrameHeader { std::uint32_t magic; std::uint16_t version; std::uint16_t flags; std::uint32_t sequence; std::uint64_t timestamp_ms; FrameType type; std::uint32_t payload_length; std::uint32_t crc32; }`
  - `struct Frame { FrameHeader header; std::vector<std::uint8_t> payload; }`
  - `std::uint32_t Crc32(std::span<const std::uint8_t> data)`
  - `std::vector<std::uint8_t> EncodeFrame(const Frame& frame)`
  - `enum class DecodeStatus { Ok, Incomplete, BadMagic, BadVersion, TooLarge, BadCrc }`
  - `struct DecodeResult { DecodeStatus status; Frame frame; std::size_t consumed; }`
  - `DecodeResult DecodeFrame(std::span<const std::uint8_t> data)`
  - `class SequenceTracker { explicit SequenceTracker(ClockPoint start); bool observe(std::uint32_t sequence, ClockPoint now); bool stalled(ClockPoint now, Duration timeout) const; std::uint32_t last_sequence() const; void reset(ClockPoint start); }`

- [ ] **Step 1: Write the failing protocol tests**

Create `native/tests/unit/supervisor_protocol_test.cpp`:

```cpp
#include "test_main.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/supervisor_protocol.h"

using htmlppt::Crc32;
using htmlppt::DecodeFrame;
using htmlppt::DecodeStatus;
using htmlppt::Duration;
using htmlppt::EncodeFrame;
using htmlppt::FakeClock;
using htmlppt::Frame;
using htmlppt::FrameType;
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
```

- [ ] **Step 2: Run the tests and verify they fail**

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: compilation fails because `core/supervisor_protocol.h` does not exist.

- [ ] **Step 3: Add the protocol header and implementation**

Create `native/src/core/supervisor_protocol.h`:

```cpp
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
```

Create `native/src/core/supervisor_protocol.cpp`:

```cpp
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

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 154/154 unit cases pass, CTest 8/8, no warnings.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/supervisor_protocol.h \
  native/src/core/supervisor_protocol.cpp \
  native/tests/unit/supervisor_protocol_test.cpp
git commit -m "feat: add supervisor protocol frames and sequence tracking"
```

---

### Task 2: Black Box and Worker Health

**Files:**
- Create: `native/src/core/blackbox.h`
- Create: `native/src/core/blackbox.cpp`
- Create: `native/src/core/worker_health.h`
- Create: `native/src/core/worker_health.cpp`
- Create: `native/tests/unit/blackbox_test.cpp`
- Create: `native/tests/unit/worker_health_test.cpp`

**Interfaces:**
- Consumes: `ClockPoint`, `Duration`, `ThreadRole`.
- Produces:
  - `enum class BlackboxEvent : std::uint16_t { StateChange=1, Event=2, Action=3, Failure=4, Heartbeat=5 }`
  - `struct BlackboxRecord { std::uint64_t sequence; std::int64_t timestamp_ms; BlackboxEvent event; std::uint16_t reserved; std::int32_t state; std::int32_t hresult; std::uint32_t pid; std::uint64_t handle; std::int64_t value; char detail[48]; }`
  - `static_assert(sizeof(BlackboxRecord) == 96)`
  - `class Blackbox { explicit Blackbox(std::size_t capacity); void Append(BlackboxRecord record); std::vector<BlackboxRecord> Snapshot() const; std::uint64_t appended() const; std::uint64_t dropped() const; std::size_t capacity() const; }`
  - `class WorkerHealthMonitor { void Register(ThreadRole, ClockPoint); void Beat(ThreadRole, ClockPoint); std::vector<ThreadRole> Stale(ClockPoint, Duration) const; bool Healthy(ClockPoint, Duration) const; std::optional<ClockPoint> LastBeat(ThreadRole) const; std::size_t registered() const; }`

- [ ] **Step 1: Write the failing black-box tests**

Create `native/tests/unit/blackbox_test.cpp`:

```cpp
#include "test_main.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/blackbox.h"

using htmlppt::Blackbox;
using htmlppt::BlackboxEvent;
using htmlppt::BlackboxRecord;

namespace {

BlackboxRecord MakeRecord(std::int64_t timestamp, const char* detail) {
  BlackboxRecord record;
  record.timestamp_ms = timestamp;
  record.event = BlackboxEvent::Event;
  record.state = 3;
  std::snprintf(record.detail, sizeof(record.detail), "%s", detail);
  return record;
}

} // namespace

HTMLPPT_TEST_CASE(blackbox_assigns_monotonic_sequences) {
  Blackbox box(4);
  box.Append(MakeRecord(10, "one"));
  box.Append(MakeRecord(20, "two"));
  const auto snapshot = box.Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(snapshot[0].sequence, std::uint64_t{1});
  HTMLPPT_CHECK_EQ(snapshot[1].sequence, std::uint64_t{2});
  HTMLPPT_CHECK_EQ(box.appended(), std::uint64_t{2});
  HTMLPPT_CHECK_EQ(box.dropped(), std::uint64_t{0});
}

HTMLPPT_TEST_CASE(blackbox_snapshot_is_chronological) {
  Blackbox box(4);
  box.Append(MakeRecord(10, "one"));
  box.Append(MakeRecord(20, "two"));
  box.Append(MakeRecord(30, "three"));
  const auto snapshot = box.Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(snapshot[0].timestamp_ms, std::int64_t{10});
  HTMLPPT_CHECK_EQ(snapshot[1].timestamp_ms, std::int64_t{20});
  HTMLPPT_CHECK_EQ(snapshot[2].timestamp_ms, std::int64_t{30});
}

HTMLPPT_TEST_CASE(blackbox_overwrites_oldest_and_counts_dropped) {
  Blackbox box(3);
  box.Append(MakeRecord(10, "one"));
  box.Append(MakeRecord(20, "two"));
  box.Append(MakeRecord(30, "three"));
  box.Append(MakeRecord(40, "four"));
  const auto snapshot = box.Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(snapshot[0].timestamp_ms, std::int64_t{20});
  HTMLPPT_CHECK_EQ(snapshot[1].timestamp_ms, std::int64_t{30});
  HTMLPPT_CHECK_EQ(snapshot[2].timestamp_ms, std::int64_t{40});
  HTMLPPT_CHECK_EQ(box.appended(), std::uint64_t{4});
  HTMLPPT_CHECK_EQ(box.dropped(), std::uint64_t{1});
}

HTMLPPT_TEST_CASE(blackbox_empty_capacity_is_safe) {
  Blackbox box(0);
  box.Append(MakeRecord(10, "one"));
  HTMLPPT_CHECK(box.Snapshot().empty());
  HTMLPPT_CHECK_EQ(box.capacity(), std::size_t{0});
}
```

- [ ] **Step 2: Write the failing worker-health tests**

Create `native/tests/unit/worker_health_test.cpp`:

```cpp
#include "test_main.h"

#include <algorithm>

#include "core/worker_health.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::ThreadRole;
using htmlppt::WorkerHealthMonitor;

namespace {

bool Contains(const std::vector<ThreadRole>& roles, ThreadRole role) {
  return std::find(roles.begin(), roles.end(), role) != roles.end();
}

} // namespace

HTMLPPT_TEST_CASE(worker_health_registered_thread_is_healthy) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::ComState, clock.now());
  HTMLPPT_CHECK(monitor.Healthy(clock.now(), Duration{1000}));
  HTMLPPT_CHECK_EQ(monitor.registered(), std::size_t{1});
}

HTMLPPT_TEST_CASE(worker_health_detects_stale_thread) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::ComState, clock.now());
  clock.advance(Duration{1000});
  HTMLPPT_CHECK(monitor.Healthy(clock.now(), Duration{1000}));
  clock.advance(Duration{1});
  const auto stale = monitor.Stale(clock.now(), Duration{1000});
  HTMLPPT_CHECK_EQ(stale.size(), std::size_t{1});
  HTMLPPT_CHECK(Contains(stale, ThreadRole::ComState));
}

HTMLPPT_TEST_CASE(worker_health_beat_refreshes_thread) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::Focus, clock.now());
  clock.advance(Duration{900});
  monitor.Beat(ThreadRole::Focus, clock.now());
  clock.advance(Duration{900});
  HTMLPPT_CHECK(monitor.Healthy(clock.now(), Duration{1000}));
}

HTMLPPT_TEST_CASE(worker_health_reports_only_stale_roles) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Register(ThreadRole::ComState, clock.now());
  monitor.Register(ThreadRole::Focus, clock.now());
  clock.advance(Duration{500});
  monitor.Beat(ThreadRole::Focus, clock.now());
  clock.advance(Duration{501});
  const auto stale = monitor.Stale(clock.now(), Duration{1000});
  HTMLPPT_CHECK_EQ(stale.size(), std::size_t{1});
  HTMLPPT_CHECK(Contains(stale, ThreadRole::ComState));
}

HTMLPPT_TEST_CASE(worker_health_unknown_beat_is_ignored) {
  FakeClock clock;
  WorkerHealthMonitor monitor;
  monitor.Beat(ThreadRole::UiOverlay, clock.now());
  HTMLPPT_CHECK(!monitor.LastBeat(ThreadRole::UiOverlay).has_value());
  HTMLPPT_CHECK_EQ(monitor.registered(), std::size_t{0});
}
```

- [ ] **Step 3: Run the tests and verify they fail**

```bash
cmake --build native/build --parallel
```

Expected: compilation fails because `core/blackbox.h` and `core/worker_health.h` do not exist.

- [ ] **Step 4: Add the black-box implementation**

Create `native/src/core/blackbox.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace htmlppt {

enum class BlackboxEvent : std::uint16_t {
  StateChange = 1,
  Event = 2,
  Action = 3,
  Failure = 4,
  Heartbeat = 5,
};

struct BlackboxRecord {
  std::uint64_t sequence = 0;
  std::int64_t timestamp_ms = 0;
  BlackboxEvent event = BlackboxEvent::Event;
  std::uint16_t reserved = 0;
  std::int32_t state = 0;
  std::int32_t hresult = 0;
  std::uint32_t pid = 0;
  std::uint64_t handle = 0;
  std::int64_t value = 0;
  char detail[48] = {};
};

static_assert(sizeof(BlackboxRecord) == 96, "blackbox record layout");

class Blackbox {
 public:
  explicit Blackbox(std::size_t capacity);

  void Append(BlackboxRecord record);
  std::vector<BlackboxRecord> Snapshot() const;
  std::uint64_t appended() const { return appended_; }
  std::uint64_t dropped() const { return dropped_; }
  std::size_t capacity() const { return records_.size(); }

 private:
  std::vector<BlackboxRecord> records_;
  std::size_t next_ = 0;
  std::uint64_t appended_ = 0;
  std::uint64_t dropped_ = 0;
};

} // namespace htmlppt
```

Create `native/src/core/blackbox.cpp`:

```cpp
#include "core/blackbox.h"

#include <algorithm>
#include <utility>

namespace htmlppt {

Blackbox::Blackbox(std::size_t capacity) : records_(capacity) {}

void Blackbox::Append(BlackboxRecord record) {
  if (records_.empty()) return;
  record.sequence = appended_ + 1;
  records_[next_] = std::move(record);
  next_ = (next_ + 1) % records_.size();
  ++appended_;
  if (appended_ > records_.size()) ++dropped_;
}

std::vector<BlackboxRecord> Blackbox::Snapshot() const {
  std::vector<BlackboxRecord> snapshot;
  if (appended_ == 0 || records_.empty()) return snapshot;
  const std::size_t count =
      std::min<std::size_t>(static_cast<std::size_t>(appended_),
                            records_.size());
  const std::size_t start =
      appended_ > records_.size() ? next_ : std::size_t{0};
  snapshot.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    snapshot.push_back(records_[(start + i) % records_.size()]);
  }
  return snapshot;
}

} // namespace htmlppt
```

- [ ] **Step 5: Add the worker-health implementation**

Create `native/src/core/worker_health.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "core/clock.h"
#include "core/types.h"

namespace htmlppt {

class WorkerHealthMonitor {
 public:
  void Register(ThreadRole role, ClockPoint now);
  void Beat(ThreadRole role, ClockPoint now);
  std::vector<ThreadRole> Stale(ClockPoint now, Duration timeout) const;
  bool Healthy(ClockPoint now, Duration timeout) const;
  std::optional<ClockPoint> LastBeat(ThreadRole role) const;
  std::size_t registered() const { return slots_.size(); }

 private:
  struct Slot {
    std::uint64_t sequence = 0;
    ClockPoint last_beat{};
  };
  std::map<ThreadRole, Slot> slots_;
};

} // namespace htmlppt
```

Create `native/src/core/worker_health.cpp`:

```cpp
#include "core/worker_health.h"

namespace htmlppt {

void WorkerHealthMonitor::Register(ThreadRole role, ClockPoint now) {
  slots_.emplace(role, Slot{0, now});
}

void WorkerHealthMonitor::Beat(ThreadRole role, ClockPoint now) {
  const auto slot = slots_.find(role);
  if (slot == slots_.end()) return;
  ++slot->second.sequence;
  slot->second.last_beat = now;
}

std::vector<ThreadRole> WorkerHealthMonitor::Stale(ClockPoint now,
                                                   Duration timeout) const {
  std::vector<ThreadRole> stale;
  for (const auto& [role, slot] : slots_) {
    // Worker health is strict: a role is stale only after the timeout has
    // been exceeded. SequenceTracker uses >= because it measures channel
    // frame absence at the exact deadline.
    if (now - slot.last_beat > timeout) stale.push_back(role);
  }
  return stale;
}

bool WorkerHealthMonitor::Healthy(ClockPoint now, Duration timeout) const {
  return Stale(now, timeout).empty();
}

std::optional<ClockPoint> WorkerHealthMonitor::LastBeat(
    ThreadRole role) const {
  const auto slot = slots_.find(role);
  if (slot == slots_.end()) return std::nullopt;
  return slot->second.last_beat;
}

} // namespace htmlppt
```

- [ ] **Step 6: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 163/163 unit cases pass, CTest 8/8, no warnings.

- [ ] **Step 7: Commit**

```bash
git add native/src/core/blackbox.h native/src/core/blackbox.cpp \
  native/src/core/worker_health.h native/src/core/worker_health.cpp \
  native/tests/unit/blackbox_test.cpp native/tests/unit/worker_health_test.cpp
git commit -m "feat: add black box and worker health monitoring"
```

---

### Task 3: Restart Policy and Supervisor Decision Engine

**Files:**
- Create: `native/src/core/supervisor.h`
- Create: `native/src/core/supervisor.cpp`
- Create: `native/tests/unit/supervisor_test.cpp`

**Interfaces:**
- Consumes: `Clock`, `ClockPoint`, `Duration`, `RestartBudget`, `SequenceTracker`.
- Produces:
  - `enum class SupervisorActionKind { StartWorker, RequestDump, TerminateWorker, RestartWorker, EnterSafeMode, RunLegacyFallback, StopComplete }`
  - `struct SupervisorAction { SupervisorActionKind kind; std::string detail; }`
  - `struct SupervisorConfig { Duration heartbeat_timeout{2000}; int max_restarts=5; Duration restart_window{600000}; std::vector<Duration> backoff{200,500,1000,2000,5000}; double jitter_ratio=0.2; std::uint32_t seed=0x9e3779b9; bool legacy_fallback=false; }`
  - `enum class WorkerEventKind { Started, Heartbeat, Exited, Hung, StopRequested, CleanupDone }`
  - `struct WorkerEvent { WorkerEventKind kind; ClockPoint at; std::uint32_t sequence=0; int exit_code=0; std::string detail; }`
  - `class SupervisorCore { SupervisorCore(SupervisorConfig, Clock&); std::vector<SupervisorAction> Start(); std::vector<SupervisorAction> Handle(const WorkerEvent&); std::vector<SupervisorAction> Tick(); std::optional<ClockPoint> NextDeadline() const; bool safe_mode() const; int restart_count() const; bool worker_running() const; bool stopping() const; }`

- [ ] **Step 1: Write the failing supervisor tests**

Create `native/tests/unit/supervisor_test.cpp`:

```cpp
#include "test_main.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "core/supervisor.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::SupervisorAction;
using htmlppt::SupervisorActionKind;
using htmlppt::SupervisorConfig;
using htmlppt::SupervisorCore;
using htmlppt::WorkerEvent;
using htmlppt::WorkerEventKind;

namespace {

bool HasAction(const std::vector<SupervisorAction>& actions,
               SupervisorActionKind kind) {
  return std::any_of(actions.begin(), actions.end(),
                     [kind](const SupervisorAction& action) {
                       return action.kind == kind;
                     });
}

SupervisorConfig Config() {
  SupervisorConfig config;
  config.jitter_ratio = 0.0;
  return config;
}

WorkerEvent Heartbeat(htmlppt::ClockPoint at, std::uint32_t sequence) {
  WorkerEvent event;
  event.kind = WorkerEventKind::Heartbeat;
  event.at = at;
  event.sequence = sequence;
  return event;
}

WorkerEvent Event(WorkerEventKind kind, htmlppt::ClockPoint at) {
  WorkerEvent event;
  event.kind = kind;
  event.at = at;
  return event;
}

void AdvanceToDeadline(SupervisorCore& core, FakeClock& clock) {
  const auto deadline = core.NextDeadline();
  HTMLPPT_CHECK(deadline.has_value());
  clock.advance(std::chrono::duration_cast<Duration>(*deadline -
                                                     clock.now()));
}

} // namespace

HTMLPPT_TEST_CASE(supervisor_start_emits_start_worker) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  const auto actions = core.Start();
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::StartWorker));
  HTMLPPT_CHECK(core.worker_running());
  HTMLPPT_CHECK(!core.safe_mode());
}

HTMLPPT_TEST_CASE(supervisor_heartbeats_keep_worker_healthy) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  core.Handle(Heartbeat(clock.now(), 1));
  clock.advance(Duration{1000});
  core.Handle(Heartbeat(clock.now(), 2));
  clock.advance(Duration{1000});
  HTMLPPT_CHECK(core.Tick().empty());
  HTMLPPT_CHECK(core.worker_running());
}

HTMLPPT_TEST_CASE(supervisor_detects_heartbeat_stall) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  clock.advance(Duration{2001});
  const auto actions = core.Tick();
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RequestDump));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::TerminateWorker));
  HTMLPPT_CHECK(!core.worker_running());
  HTMLPPT_CHECK(core.NextDeadline().has_value());
}

HTMLPPT_TEST_CASE(supervisor_restarts_after_backoff) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  clock.advance(Duration{2001});
  core.Tick();
  AdvanceToDeadline(core, clock);
  const auto actions = core.Tick();
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RestartWorker));
  HTMLPPT_CHECK(core.worker_running());
  HTMLPPT_CHECK_EQ(core.restart_count(), 1);
}

HTMLPPT_TEST_CASE(supervisor_uses_escalating_backoff) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const std::vector<long long> expected{200, 500, 1000, 2000, 5000};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    clock.advance(Duration{2001});
    const auto stall = core.Tick();
    HTMLPPT_CHECK(HasAction(stall, SupervisorActionKind::RequestDump));
    const auto deadline = core.NextDeadline();
    HTMLPPT_CHECK(deadline.has_value());
    HTMLPPT_CHECK_EQ(
        std::chrono::duration_cast<Duration>(*deadline - clock.now()).count(),
        expected[i]);
    AdvanceToDeadline(core, clock);
    const auto restart = core.Tick();
    HTMLPPT_CHECK(HasAction(restart, SupervisorActionKind::RestartWorker));
  }
}

HTMLPPT_TEST_CASE(supervisor_budget_exhaustion_enters_safe_mode) {
  FakeClock clock;
  auto config = Config();
  config.legacy_fallback = true;
  SupervisorCore core(config, clock);
  core.Start();
  for (int i = 0; i < 5; ++i) {
    clock.advance(Duration{2001});
    core.Tick();
    AdvanceToDeadline(core, clock);
    core.Tick();
  }
  clock.advance(Duration{2001});
  const auto actions = core.Tick();
  HTMLPPT_CHECK(core.safe_mode());
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::EnterSafeMode));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RunLegacyFallback));
}

HTMLPPT_TEST_CASE(supervisor_worker_exit_requests_dump_and_restart) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const auto actions = core.Handle(Event(WorkerEventKind::Exited, clock.now()));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RequestDump));
  HTMLPPT_CHECK(!core.worker_running());
  HTMLPPT_CHECK(core.NextDeadline().has_value());
}

HTMLPPT_TEST_CASE(supervisor_hung_event_requests_dump_and_terminate) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const auto actions = core.Handle(Event(WorkerEventKind::Hung, clock.now()));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::RequestDump));
  HTMLPPT_CHECK(HasAction(actions, SupervisorActionKind::TerminateWorker));
  HTMLPPT_CHECK(!core.worker_running());
}

HTMLPPT_TEST_CASE(supervisor_duplicate_hung_does_not_consume_budget) {
  FakeClock clock;
  auto config = Config();
  config.max_restarts = 1;
  SupervisorCore core(config, clock);
  core.Start();
  core.Handle(Event(WorkerEventKind::Hung, clock.now()));
  const auto duplicate =
      core.Handle(Event(WorkerEventKind::Hung, clock.now()));
  HTMLPPT_CHECK(duplicate.empty());
  HTMLPPT_CHECK(!core.safe_mode());
}

HTMLPPT_TEST_CASE(supervisor_stop_terminates_and_completes) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  const auto stop =
      core.Handle(Event(WorkerEventKind::StopRequested, clock.now()));
  HTMLPPT_CHECK(HasAction(stop, SupervisorActionKind::TerminateWorker));
  HTMLPPT_CHECK(core.stopping());
  const auto done = core.Handle(Event(WorkerEventKind::CleanupDone, clock.now()));
  HTMLPPT_CHECK(HasAction(done, SupervisorActionKind::StopComplete));
}

HTMLPPT_TEST_CASE(supervisor_stale_heartbeat_after_stop_is_ignored) {
  FakeClock clock;
  SupervisorCore core(Config(), clock);
  core.Start();
  core.Handle(Event(WorkerEventKind::StopRequested, clock.now()));
  clock.advance(Duration{3000});
  HTMLPPT_CHECK(core.Tick().empty());
}
```

- [ ] **Step 2: Run the tests and verify they fail**

```bash
cmake --build native/build --parallel
```

Expected: compilation fails because `core/supervisor.h` does not exist.

- [ ] **Step 3: Add the supervisor header**

Create `native/src/core/supervisor.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/retry_policy.h"
#include "core/supervisor_protocol.h"

namespace htmlppt {

enum class SupervisorActionKind {
  StartWorker,
  RequestDump,
  TerminateWorker,
  RestartWorker,
  EnterSafeMode,
  RunLegacyFallback,
  StopComplete,
};

struct SupervisorAction {
  SupervisorActionKind kind = SupervisorActionKind::StartWorker;
  std::string detail;
};

struct SupervisorConfig {
  Duration heartbeat_timeout{2000};
  int max_restarts = 5;
  Duration restart_window{600000};
  std::vector<Duration> backoff{
      Duration{200}, Duration{500}, Duration{1000}, Duration{2000},
      Duration{5000}};
  double jitter_ratio = 0.2;
  std::uint32_t seed = 0x9e3779b9u;
  bool legacy_fallback = false;
};

enum class WorkerEventKind {
  Started,
  Heartbeat,
  Exited,
  Hung,
  StopRequested,
  CleanupDone,
};

struct WorkerEvent {
  WorkerEventKind kind = WorkerEventKind::Heartbeat;
  ClockPoint at{};
  std::uint32_t sequence = 0;
  int exit_code = 0;
  std::string detail;
};

class SupervisorCore {
 public:
  SupervisorCore(SupervisorConfig config, Clock& clock);

  std::vector<SupervisorAction> Start();
  std::vector<SupervisorAction> Handle(const WorkerEvent& event);
  std::vector<SupervisorAction> Tick();
  std::optional<ClockPoint> NextDeadline() const { return restart_deadline_; }
  bool safe_mode() const { return safe_mode_; }
  int restart_count() const { return restart_count_; }
  bool worker_running() const { return worker_running_; }
  bool stopping() const { return stopping_; }

 private:
  std::optional<Duration> BackoffFor(int attempt);
  std::uint32_t NextRandom();
  std::vector<SupervisorAction> EnterSafeMode(std::string detail);
  std::vector<SupervisorAction> ScheduleRestart(ClockPoint at,
                                                std::string detail);

  SupervisorConfig config_;
  Clock& clock_;
  SequenceTracker sequence_;
  RestartBudget budget_;
  bool worker_running_ = false;
  bool stopping_ = false;
  bool safe_mode_ = false;
  int restart_count_ = 0;
  std::optional<ClockPoint> restart_deadline_;
  std::uint32_t random_state_ = 0x9e3779b9u;
};

} // namespace htmlppt
```

- [ ] **Step 4: Add the supervisor implementation**

Create `native/src/core/supervisor.cpp`:

```cpp
#include "core/supervisor.h"

#include <utility>

namespace htmlppt {

SupervisorCore::SupervisorCore(SupervisorConfig config, Clock& clock)
    : config_(std::move(config)),
      clock_(clock),
      sequence_(clock_.now()),
      budget_(config_.max_restarts, config_.restart_window),
      random_state_(config_.seed == 0 ? 0x9e3779b9u : config_.seed) {}

std::vector<SupervisorAction> SupervisorCore::Start() {
  if (worker_running_ || safe_mode_ || stopping_) return {};
  worker_running_ = true;
  sequence_.reset(clock_.now());
  return {{SupervisorActionKind::StartWorker, "start worker"}};
}

std::vector<SupervisorAction> SupervisorCore::Handle(
    const WorkerEvent& event) {
  if (safe_mode_) {
    if (event.kind == WorkerEventKind::StopRequested) {
      return {{SupervisorActionKind::StopComplete, "safe mode stop"}};
    }
    return {};
  }
  switch (event.kind) {
    case WorkerEventKind::Started:
      if (stopping_) return {};
      worker_running_ = true;
      sequence_.reset(event.at);
      return {};
    case WorkerEventKind::Heartbeat:
      if (!worker_running_ || stopping_) return {};
      sequence_.observe(event.sequence, event.at);
      return {};
    case WorkerEventKind::Exited: {
      if (!worker_running_) return {};
      worker_running_ = false;
      if (stopping_) return {};
      auto actions = std::vector<SupervisorAction>{
          {SupervisorActionKind::RequestDump, "worker exited"}};
      auto scheduled = ScheduleRestart(event.at, "worker exited");
      actions.insert(actions.end(), scheduled.begin(), scheduled.end());
      return actions;
    }
    case WorkerEventKind::Hung: {
      if (!worker_running_ || stopping_) return {};
      worker_running_ = false;
      auto actions = std::vector<SupervisorAction>{
          {SupervisorActionKind::RequestDump, "worker hung"},
          {SupervisorActionKind::TerminateWorker, "worker hung"}};
      auto scheduled = ScheduleRestart(event.at, "worker hung");
      actions.insert(actions.end(), scheduled.begin(), scheduled.end());
      return actions;
    }
    case WorkerEventKind::StopRequested:
      stopping_ = true;
      restart_deadline_.reset();
      if (worker_running_) {
        return {{SupervisorActionKind::TerminateWorker, "stop requested"}};
      }
      return {{SupervisorActionKind::StopComplete, "stop requested"}};
    case WorkerEventKind::CleanupDone:
      if (!stopping_) return {};
      worker_running_ = false;
      return {{SupervisorActionKind::StopComplete, "cleanup done"}};
  }
  return {};
}

std::vector<SupervisorAction> SupervisorCore::Tick() {
  const auto now = clock_.now();
  if (safe_mode_ || stopping_) return {};
  if (restart_deadline_.has_value()) {
    if (now < *restart_deadline_) return {};
    restart_deadline_.reset();
    sequence_.reset(now);
    worker_running_ = true;
    ++restart_count_;
    return {{SupervisorActionKind::RestartWorker, "restart worker"}};
  }
  if (worker_running_ &&
      sequence_.stalled(now, config_.heartbeat_timeout)) {
    worker_running_ = false;
    auto actions = std::vector<SupervisorAction>{
        {SupervisorActionKind::RequestDump, "heartbeat stalled"},
        {SupervisorActionKind::TerminateWorker, "heartbeat stalled"}};
    auto scheduled = ScheduleRestart(now, "heartbeat stalled");
    actions.insert(actions.end(), scheduled.begin(), scheduled.end());
    return actions;
  }
  return {};
}

std::optional<Duration> SupervisorCore::BackoffFor(int attempt) {
  if (attempt < 0 ||
      static_cast<std::size_t>(attempt) >= config_.backoff.size()) {
    return std::nullopt;
  }
  Duration delay = config_.backoff[static_cast<std::size_t>(attempt)];
  if (config_.jitter_ratio > 0.0) {
    const auto max_jitter = static_cast<long long>(
        static_cast<double>(delay.count()) * config_.jitter_ratio);
    if (max_jitter > 0) {
      const auto span = static_cast<std::uint64_t>(2 * max_jitter + 1);
      const auto offset =
          static_cast<long long>(NextRandom() % span) - max_jitter;
      delay += Duration{offset};
      if (delay < Duration{0}) delay = Duration{0};
    }
  }
  return delay;
}

std::uint32_t SupervisorCore::NextRandom() {
  random_state_ ^= random_state_ << 13;
  random_state_ ^= random_state_ >> 17;
  random_state_ ^= random_state_ << 5;
  return random_state_;
}

std::vector<SupervisorAction> SupervisorCore::EnterSafeMode(
    std::string detail) {
  safe_mode_ = true;
  worker_running_ = false;
  restart_deadline_.reset();
  std::vector<SupervisorAction> actions;
  actions.push_back({SupervisorActionKind::EnterSafeMode, std::move(detail)});
  if (config_.legacy_fallback) {
    actions.push_back(
        {SupervisorActionKind::RunLegacyFallback, "safe mode fallback"});
  }
  return actions;
}

std::vector<SupervisorAction> SupervisorCore::ScheduleRestart(
    ClockPoint at, std::string detail) {
  if (!budget_.allow(at)) {
    return EnterSafeMode(std::move(detail) + ": restart budget exhausted");
  }
  const auto delay = BackoffFor(restart_count_);
  if (!delay.has_value()) {
    return EnterSafeMode(std::move(detail) + ": no backoff available");
  }
  budget_.record(at);
  restart_deadline_ = at + *delay;
  return {};
}

} // namespace htmlppt
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 174/174 unit cases pass, CTest 8/8, no warnings.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/supervisor.h native/src/core/supervisor.cpp \
  native/tests/unit/supervisor_test.cpp
git commit -m "feat: add supervisor restart policy and decision engine"
```

---

### Task 4: Single-Instance Identity, Journal Reconciliation, Docs, and Final Gate

**Files:**
- Create: `native/src/core/single_instance.h`
- Create: `native/src/core/single_instance.cpp`
- Create: `native/src/core/journal_reconcile.h`
- Create: `native/src/core/journal_reconcile.cpp`
- Create: `native/tests/unit/single_instance_test.cpp`
- Create: `native/tests/unit/journal_reconcile_test.cpp`
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-15-cpp-watchdog-supervisor-core.md` (as-built)

**Interfaces:**
- Consumes: `JournalRecord`, `std::filesystem::path`, nlohmann/json.
- Produces:
  - `struct InstanceIdentity { std::string fingerprint; std::string mutex_name; }`
  - `struct LockInfo { std::string run_id; int pid=0; std::string config_hash; }`
  - `std::string StableHash16(std::string_view text)`
  - `InstanceIdentity MakeInstanceIdentity(const std::filesystem::path& config_dir, std::string_view config_hash)`
  - `std::string FormatLockLine(const LockInfo&)`
  - `std::optional<LockInfo> ParseLockLine(std::string_view)`
  - `bool IsLegacyLock(std::string_view)`
  - `enum class ReconcileOutcome { Clean, TakeOver, Cleanup, RestoreWps, SafeMode }`
  - `struct ReconcileFacts { bool edge_process_alive; bool overlay_owned; bool can_restore_wps; bool flags_owned_by_current_run; }`
  - `struct ReconcileDecision { ReconcileOutcome outcome; bool kill_edge; bool hide_overlay; bool resume_wps; int target_slide; std::string reason; }`
  - `ReconcileDecision ReconcileStartup(const std::vector<JournalRecord>& tail, const ReconcileFacts& facts, std::string_view run_id)`

- [ ] **Step 1: Write the failing single-instance tests**

Create `native/tests/unit/single_instance_test.cpp`:

```cpp
#include "test_main.h"

#include <string>

#include "core/single_instance.h"

using htmlppt::FormatLockLine;
using htmlppt::IsLegacyLock;
using htmlppt::LockInfo;
using htmlppt::MakeInstanceIdentity;
using htmlppt::ParseLockLine;
using htmlppt::StableHash16;

HTMLPPT_TEST_CASE(instance_identity_is_stable_and_path_sensitive) {
  const auto first = MakeInstanceIdentity("/tmp/demo-a", "hash-1");
  const auto second = MakeInstanceIdentity("/tmp/demo-a", "hash-1");
  const auto third = MakeInstanceIdentity("/tmp/demo-b", "hash-1");
  HTMLPPT_CHECK_EQ(first.fingerprint, second.fingerprint);
  HTMLPPT_CHECK(first.fingerprint != third.fingerprint);
  HTMLPPT_CHECK_EQ(first.fingerprint.size(), std::size_t{16});
  HTMLPPT_CHECK_EQ(first.mutex_name,
                   std::string{"Local\\htmlppt-watchdog-"} + first.fingerprint);
}

HTMLPPT_TEST_CASE(stable_hash_is_16_hex_and_config_sensitive) {
  const auto first = StableHash16("alpha");
  const auto second = StableHash16("beta");
  HTMLPPT_CHECK_EQ(first.size(), std::size_t{16});
  HTMLPPT_CHECK(first != second);
}

HTMLPPT_TEST_CASE(lock_line_round_trips) {
  LockInfo info;
  info.run_id = "run-1";
  info.pid = 4242;
  info.config_hash = "abc123";
  const std::string line = FormatLockLine(info);
  const auto parsed = ParseLockLine(line);
  HTMLPPT_CHECK(parsed.has_value());
  HTMLPPT_CHECK_EQ(parsed->run_id, std::string{"run-1"});
  HTMLPPT_CHECK_EQ(parsed->pid, 4242);
  HTMLPPT_CHECK_EQ(parsed->config_hash, std::string{"abc123"});
  HTMLPPT_CHECK(!IsLegacyLock(line));
}

HTMLPPT_TEST_CASE(legacy_lock_detection) {
  HTMLPPT_CHECK(IsLegacyLock("not json"));
  HTMLPPT_CHECK(IsLegacyLock(R"({"pid": 42})"));
  HTMLPPT_CHECK(IsLegacyLock(R"({"run_id": "", "pid": 42})"));
  HTMLPPT_CHECK(!IsLegacyLock(R"({"run_id":"r","pid":42,"config_hash":"h"})"));
}
```

- [ ] **Step 2: Write the failing reconciliation tests**

Create `native/tests/unit/journal_reconcile_test.cpp`:

```cpp
#include "test_main.h"

#include <string>
#include <utility>

#include "core/journal_reconcile.h"

using htmlppt::JournalRecord;
using htmlppt::ReconcileDecision;
using htmlppt::ReconcileFacts;
using htmlppt::ReconcileOutcome;

namespace {

JournalRecord Record(std::string run_id, std::string state, int slide) {
  JournalRecord record;
  record.run_id = std::move(run_id);
  record.state = std::move(state);
  record.slide = slide;
  return record;
}

} // namespace

HTMLPPT_TEST_CASE(reconcile_empty_journal_is_clean) {
  const auto decision =
      htmlppt::ReconcileStartup({}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
}

HTMLPPT_TEST_CASE(reconcile_monitoring_is_clean) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Monitoring", 0)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Clean);
}

HTMLPPT_TEST_CASE(reconcile_live_restores_wps_when_possible) {
  ReconcileFacts facts;
  facts.can_restore_wps = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Live", 5)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::RestoreWps);
  HTMLPPT_CHECK(decision.resume_wps);
  HTMLPPT_CHECK_EQ(decision.target_slide, 5);
}

HTMLPPT_TEST_CASE(reconcile_live_cleans_up_leftover_edge) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Live", 5)}, facts, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Cleanup);
  HTMLPPT_CHECK(decision.kill_edge);
}

HTMLPPT_TEST_CASE(reconcile_different_run_cleans_up) {
  ReconcileFacts facts;
  facts.edge_process_alive = true;
  facts.overlay_owned = true;
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-old", "Live", 5)}, facts, "run-new");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::Cleanup);
  HTMLPPT_CHECK(decision.kill_edge);
  HTMLPPT_CHECK(decision.hide_overlay);
}

HTMLPPT_TEST_CASE(reconcile_faulted_enters_safe_mode) {
  const auto decision = htmlppt::ReconcileStartup(
      {Record("run-1", "Faulted", 5)}, ReconcileFacts{}, "run-1");
  HTMLPPT_CHECK(decision.outcome == ReconcileOutcome::SafeMode);
}
```

- [ ] **Step 3: Run the tests and verify they fail**

```bash
cmake --build native/build --parallel
```

Expected: compilation fails because the new headers do not exist.

- [ ] **Step 4: Add the single-instance implementation**

Create `native/src/core/single_instance.h`:

```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace htmlppt {

struct InstanceIdentity {
  std::string fingerprint;
  std::string mutex_name;
};

struct LockInfo {
  std::string run_id;
  int pid = 0;
  std::string config_hash;
};

std::string StableHash16(std::string_view text);
InstanceIdentity MakeInstanceIdentity(const std::filesystem::path& config_dir,
                                      std::string_view config_hash);
std::string FormatLockLine(const LockInfo& info);
std::optional<LockInfo> ParseLockLine(std::string_view line);
bool IsLegacyLock(std::string_view line);

} // namespace htmlppt
```

Create `native/src/core/single_instance.cpp`:

```cpp
#include "core/single_instance.h"

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace htmlppt {

std::string StableHash16(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char character : text) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
      << hash;
  return out.str();
}

InstanceIdentity MakeInstanceIdentity(const std::filesystem::path& config_dir,
                                      std::string_view config_hash) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(config_dir, error);
  if (error) canonical = config_dir;
  const std::string seed =
      canonical.generic_string() + "|" + std::string(config_hash);
  InstanceIdentity identity;
  identity.fingerprint = StableHash16(seed);
  identity.mutex_name =
      "Local\\htmlppt-watchdog-" + identity.fingerprint;
  return identity;
}

std::string FormatLockLine(const LockInfo& info) {
  nlohmann::ordered_json json;
  json["run_id"] = info.run_id;
  json["pid"] = info.pid;
  json["config_hash"] = info.config_hash;
  return json.dump();
}

std::optional<LockInfo> ParseLockLine(std::string_view line) {
  try {
    const auto json = nlohmann::json::parse(line.begin(), line.end());
    if (!json.is_object()) return std::nullopt;
    const auto run_id = json.find("run_id");
    const auto pid = json.find("pid");
    const auto config_hash = json.find("config_hash");
    if (run_id == json.end() || !run_id->is_string() ||
        run_id->get<std::string>().empty()) {
      return std::nullopt;
    }
    if (pid == json.end() || !pid->is_number_integer()) return std::nullopt;
    const auto pid_value = pid->get<std::int64_t>();
    if (pid_value <= 0 ||
        pid_value > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    if (config_hash == json.end() || !config_hash->is_string()) {
      return std::nullopt;
    }
    LockInfo info;
    info.run_id = run_id->get<std::string>();
    info.pid = static_cast<int>(pid_value);
    info.config_hash = config_hash->get<std::string>();
    return info;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool IsLegacyLock(std::string_view line) {
  return !ParseLockLine(line).has_value();
}

} // namespace htmlppt
```

- [ ] **Step 5: Add the reconciliation implementation**

Create `native/src/core/journal_reconcile.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/journal.h"

namespace htmlppt {

enum class ReconcileOutcome {
  Clean,
  TakeOver,
  Cleanup,
  RestoreWps,
  SafeMode,
};

struct ReconcileFacts {
  bool edge_process_alive = false;
  bool overlay_owned = false;
  bool can_restore_wps = false;
  bool flags_owned_by_current_run = false;
};

struct ReconcileDecision {
  ReconcileOutcome outcome = ReconcileOutcome::Clean;
  bool kill_edge = false;
  bool hide_overlay = false;
  bool resume_wps = false;
  int target_slide = 0;
  std::string reason;
};

ReconcileDecision ReconcileStartup(const std::vector<JournalRecord>& tail,
                                   const ReconcileFacts& facts,
                                   std::string_view run_id);

} // namespace htmlppt
```

Create `native/src/core/journal_reconcile.cpp`:

```cpp
#include "core/journal_reconcile.h"

#include <algorithm>
#include <utility>

namespace htmlppt {
namespace {

const JournalRecord* LastStateRecord(const std::vector<JournalRecord>& tail) {
  for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
    if (!it->state.empty()) return &*it;
  }
  return nullptr;
}

ReconcileDecision Clean(std::string reason) {
  ReconcileDecision decision;
  decision.outcome = ReconcileOutcome::Clean;
  decision.reason = std::move(reason);
  return decision;
}

} // namespace

ReconcileDecision ReconcileStartup(const std::vector<JournalRecord>& tail,
                                   const ReconcileFacts& facts,
                                   std::string_view run_id) {
  const JournalRecord* last = LastStateRecord(tail);
  if (last == nullptr) return Clean("empty journal");

  ReconcileDecision decision;
  decision.target_slide = last->slide;
  const bool same_run = std::string_view(last->run_id) == run_id;

  if (!same_run) {
    if (facts.flags_owned_by_current_run) {
      decision.outcome = ReconcileOutcome::TakeOver;
      decision.kill_edge = facts.edge_process_alive;
      decision.hide_overlay = facts.overlay_owned;
      decision.resume_wps = facts.can_restore_wps;
      decision.reason = "take over flags from a previous run";
      return decision;
    }
    decision.outcome = ReconcileOutcome::Cleanup;
    decision.kill_edge = facts.edge_process_alive;
    decision.hide_overlay = facts.overlay_owned;
    decision.resume_wps = facts.can_restore_wps;
    decision.reason = "previous run left state behind";
    return decision;
  }

  if (last->state == "Live" || last->state == "Returning") {
    if (facts.can_restore_wps) {
      decision.outcome = ReconcileOutcome::RestoreWps;
      decision.resume_wps = true;
      decision.reason = "restore WPS after an interrupted live/return state";
      return decision;
    }
    if (facts.edge_process_alive || facts.overlay_owned) {
      decision.outcome = ReconcileOutcome::Cleanup;
      decision.kill_edge = facts.edge_process_alive;
      decision.hide_overlay = facts.overlay_owned;
      decision.reason = "clean up an interrupted live/return state";
      return decision;
    }
    return Clean("live/return state has no leftover resources");
  }

  if (last->state == "CoverIn" || last->state == "EdgeStarting" ||
      last->state == "ShowExit") {
    decision.outcome = ReconcileOutcome::Cleanup;
    decision.kill_edge = facts.edge_process_alive;
    decision.hide_overlay = facts.overlay_owned;
    decision.resume_wps = facts.can_restore_wps;
    decision.reason = "clean up an interrupted switch";
    return decision;
  }

  if (last->state == "Faulted" || last->state == "SafeMode") {
    decision.outcome = ReconcileOutcome::SafeMode;
    decision.reason = "previous run ended in " + last->state;
    return decision;
  }

  decision.kill_edge = facts.edge_process_alive;
  decision.hide_overlay = facts.overlay_owned;
  decision.reason = "previous state is safe to resume";
  return decision;
}

} // namespace htmlppt
```

- [ ] **Step 6: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 184/184 unit cases pass, CTest 8/8, no warnings.

- [ ] **Step 7: Document the supervisor core and run the final gate**

Append to `native/README.md`:

````markdown
## Supervisor core (Plan 4A)

`core/supervisor_protocol` defines the worker-to-supervisor wire format: frame
magic/version/sequence/timestamp/type/length plus CRC32 over the header and
payload, with a 64 KiB payload limit. `SequenceTracker` reports heartbeat
stalls.

`core/blackbox` is a fixed 96-byte-record ring buffer; Plan 4B maps it onto
shared memory. `core/worker_health` tracks registered thread roles and reports
stale heartbeats.

`core/supervisor` is a declarative decision engine. It emits `StartWorker`,
`RequestDump`, `TerminateWorker`, `RestartWorker`, `EnterSafeMode`,
`RunLegacyFallback`, and `StopComplete`. It uses a 2 s heartbeat timeout, a
5-restarts-per-10-minutes budget, and a 200 ms/500 ms/1 s/2 s/5 s backoff with
jitter. `NextDeadline()` exposes the pending restart; `Tick()` dispatches it.

`core/single_instance` derives a stable instance fingerprint and mutex name
from the canonical config directory plus config hash, formats the
`watcher.lock` JSON line, and recognizes legacy lock lines.
`core/journal_reconcile` decides whether startup should clean up, take over,
restore WPS, or enter SafeMode from the last journal state and observed
resources.

Plan 4B maps these cores onto Windows Job Objects, named pipes, shared memory,
MiniDump, process launch/termination, and CLI mode dispatch. No Windows API is
used in Plan 4A.
````

Run the final gate:

```bash
export PATH="$HOME/.local/bin:$PATH"
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
CHROME_BIN=/home/bingmoe/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome npm test
```

Expected: 184/184 unit cases, CTest 8/8, npm 3/3, `-Werror` clean.

- [ ] **Step 8: Record as-built amendments and commit**

Append an `As-Built Amendments (2026-11-15)` section to this plan with the actual counts, deviations, and deferred items assigned to Plan 4B/5/6/8. Record the Task 3 review fix: duplicate `Hung` events during restart backoff are ignored and do not consume the restart budget. Then:

```bash
git add native/src/core/single_instance.h native/src/core/single_instance.cpp \
  native/src/core/journal_reconcile.h native/src/core/journal_reconcile.cpp \
  native/tests/unit/single_instance_test.cpp \
  native/tests/unit/journal_reconcile_test.cpp native/README.md \
  docs/superpowers/plans/2026-11-15-cpp-watchdog-supervisor-core.md
git commit -m "feat: add supervisor identity and startup reconciliation"
```

---

## Self-Review

### Plan 4A coverage

| Spec requirement | Plan 4A coverage |
|---|---|
| Frame format: magic, version, sequence, timestamp, type, length, CRC32 | Task 1 |
| Sequence monotonicity and heartbeat stall detection | Task 1 `SequenceTracker` |
| Shared-memory-compatible fixed-record black box | Task 2 `BlackboxRecord` / `Blackbox` |
| Per-thread heartbeat slots and stale detection | Task 2 `WorkerHealthMonitor` |
| Supervisor heartbeat timeout 2s | Task 3 `SupervisorConfig` / `Tick` |
| Restart budget 5 per 10 minutes | Task 3 `RestartBudget` |
| Backoff 200ms/500ms/1s/2s/5s with jitter | Task 3 `BackoffFor` |
| Supervisor actions: start, dump, terminate, restart, safe mode, legacy, stop complete | Task 3 |
| Single instance key from config directory + config hash | Task 4 |
| `watcher.lock` JSON line and legacy detection | Task 4 |
| Startup reconciliation: last state, Edge/overlay ownership, WPS restore, SafeMode | Task 4 |
| Platform neutrality on Linux/WSL | Tasks 1-4; no Windows headers or APIs |
| Existing suite stays green | Every task; final gate 184/184 |

Deferred to Plan 4B: Windows Job Object, named pipe framing transport, shared-memory mapping, MiniDump helper, process launch/termination, single-instance mutex creation, actual journal I/O wiring, and CLI `--supervisor`/`--worker`/`--stop`/`--status`/`--dump-helper`. Deferred to Plan 5: WPS COM host, overlay, focus, Edge execution. Deferred to Plan 6: event-driven scheduling, thread priorities, power policy, fault injection. Deferred to Plan 8: Windows end-to-end acceptance and release.

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.

### Type consistency

- `Frame`, `DecodeResult`, and `SequenceTracker` are defined in Task 1 and consumed unchanged by Task 3.
- `BlackboxRecord` layout is fixed at 96 bytes for Plan 4B shared memory.
- `WorkerHealthMonitor` uses the Plan 1 `ThreadRole` enum.
- `SupervisorCore` actions are the same enum values referenced by Task 3 tests.
- `SingleInstance` uses `std::string` for the fingerprint and mutex name; `ReconcileStartup` uses the Plan 1 `JournalRecord`.
- Final count: 145 baseline + 9 protocol + 9 blackbox/health + 11 supervisor + 10 identity/reconcile = 184.

---

## Execution Handoff

Plan 4A is ready. Execute Tasks 1-4 in order. Each task ends with a green full suite and a commit. Task 4 records the final gate and as-built amendments.
