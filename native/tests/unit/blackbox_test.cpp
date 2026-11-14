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
