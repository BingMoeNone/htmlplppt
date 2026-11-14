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
