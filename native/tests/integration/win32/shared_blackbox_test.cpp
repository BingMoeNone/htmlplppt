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

// The on-disk header SharedBlackbox uses, replicated so a test can forge one.
// Keep it in sync with the private struct (the compiler checks the size below).
struct ForgedHeader {
  std::uint32_t magic = 0x424B4C42u;
  std::uint16_t version = 1;
  std::uint16_t record_size = 96;
  std::uint32_t capacity = 1;
  std::uint32_t reserved = 0;
  std::uint64_t mapping_bytes = 0;
  std::atomic<std::uint64_t> appended{0};
  std::atomic<std::uint64_t> dropped{0};
};
static_assert(sizeof(ForgedHeader) == 40, "forged header layout");

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
  reader->Append(MakeRecord(99));
  HTMLPPT_CHECK_EQ(reader->Snapshot().size(), std::size_t{3});
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

HTMLPPT_TEST_CASE(win32_shared_blackbox_latest_reads_only_the_last_record) {
  const std::wstring name = UniqueMappingName();
  auto box = SharedBlackbox::Create(name, 3);
  HTMLPPT_CHECK(box != nullptr);
  HTMLPPT_CHECK(!box->Latest().has_value());
  box->Append(MakeRecord(10));
  box->Append(MakeRecord(20));
  const auto latest = box->Latest();
  HTMLPPT_CHECK(latest.has_value());
  HTMLPPT_CHECK_EQ(latest->timestamp_ms, std::int64_t{20});
  HTMLPPT_CHECK_EQ(latest->sequence, std::uint64_t{2});
  // After the ring wraps the newest slot still answers, without copying the ring.
  box->Append(MakeRecord(30));
  box->Append(MakeRecord(40));
  const auto wrapped = box->Latest();
  HTMLPPT_CHECK(wrapped.has_value());
  HTMLPPT_CHECK_EQ(wrapped->timestamp_ms, std::int64_t{40});
  HTMLPPT_CHECK_EQ(wrapped->sequence, std::uint64_t{4});
  // A read-only view answers too.
  auto reader = SharedBlackbox::Open(name);
  HTMLPPT_CHECK(reader != nullptr);
  const auto from_reader = reader->Latest();
  HTMLPPT_CHECK(from_reader.has_value());
  HTMLPPT_CHECK_EQ(from_reader->timestamp_ms, std::int64_t{40});
}

HTMLPPT_TEST_CASE(win32_shared_blackbox_rejects_a_forged_header) {
  // A same-user process can create the mapping name first and publish a header
  // of its choosing. Open must not trust `capacity`: a ring longer than the
  // section would read past the view on the next Snapshot/Latest.
  const std::wstring name = UniqueMappingName();
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0, 4096, name.c_str());
  HTMLPPT_CHECK(mapping != nullptr);
  auto* header =
      static_cast<ForgedHeader*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0,
                                               0, 0));
  HTMLPPT_CHECK(header != nullptr);
  new (header) ForgedHeader{};
  header->capacity = 0xFFFFFFFFu;
  header->mapping_bytes = 4096;
  HTMLPPT_CHECK(SharedBlackbox::Open(name) == nullptr);

  header->capacity = 4;
  header->mapping_bytes = 1ull << 40;
  HTMLPPT_CHECK(SharedBlackbox::Open(name) == nullptr);

  UnmapViewOfFile(header);
  CloseHandle(mapping);
}

#endif // _WIN32
