#include "test_main.h"

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "core/blackbox.h"
#include "platform/win32/shared_blackbox.h"

using htmlppt::BlackboxEvent;
using htmlppt::BlackboxRecord;
using htmlppt::SharedBlackbox;

namespace {

std::wstring UniqueMappingName() {
  static std::atomic<std::uint32_t> counter{0};
  return L"Local\\htmlppt-test-blackbox-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(counter++);
}

BlackboxRecord MakeRecord(std::int64_t timestamp) {
  BlackboxRecord record;
  record.timestamp_ms = timestamp;
  record.event = BlackboxEvent::Event;
  return record;
}

} // namespace

HTMLPPT_TEST_CASE(win32_shared_blackbox_round_trip) {
  const std::wstring name = UniqueMappingName();
  auto writer = SharedBlackbox::Create(name, 8);
  HTMLPPT_CHECK(writer != nullptr);
  writer->Append(MakeRecord(10));
  writer->Append(MakeRecord(20));
  writer->Append(MakeRecord(30));
  HTMLPPT_CHECK_EQ(writer->appended(), std::uint64_t{3});
  auto reader = SharedBlackbox::Open(name);
  HTMLPPT_CHECK(reader != nullptr);
  const auto snapshot = reader->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(snapshot[0].timestamp_ms, std::int64_t{10});
  HTMLPPT_CHECK_EQ(snapshot[2].sequence, std::uint64_t{3});
}

HTMLPPT_TEST_CASE(win32_shared_blackbox_wraps_and_counts_dropped) {
  const std::wstring name = UniqueMappingName();
  auto box = SharedBlackbox::Create(name, 3);
  HTMLPPT_CHECK(box != nullptr);
  box->Append(MakeRecord(10));
  box->Append(MakeRecord(20));
  box->Append(MakeRecord(30));
  box->Append(MakeRecord(40));
  const auto snapshot = box->Snapshot();
  HTMLPPT_CHECK_EQ(snapshot.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(snapshot[0].timestamp_ms, std::int64_t{20});
  HTMLPPT_CHECK_EQ(snapshot[2].timestamp_ms, std::int64_t{40});
  HTMLPPT_CHECK_EQ(box->dropped(), std::uint64_t{1});
}

HTMLPPT_TEST_CASE(win32_shared_blackbox_open_missing_returns_null) {
  const std::wstring name = UniqueMappingName();
  HTMLPPT_CHECK(SharedBlackbox::Open(name) == nullptr);
}

#endif // _WIN32
