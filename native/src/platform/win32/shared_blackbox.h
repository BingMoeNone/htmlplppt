#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/blackbox.h"

namespace htmlppt {

#ifdef _WIN32

// Shared-memory black-box ring. Append is single-writer: the caller must
// serialize all writes (Plan 4B-2 uses the worker as the only writer).
// Snapshot stays within the mapping during a concurrent wrap, but the returned
// records may be torn or out of order and are diagnostic only. Create returns
// a writable mapping; Open returns a read-only mapping whose Append is a no-op;
// OpenWritable attaches a writable view for the worker.
class SharedBlackbox {
 public:
  static std::unique_ptr<SharedBlackbox> Create(const std::wstring& name,
                                                std::size_t capacity);
  static std::unique_ptr<SharedBlackbox> Open(const std::wstring& name);
  static std::unique_ptr<SharedBlackbox> OpenWritable(const std::wstring& name);
  ~SharedBlackbox();

  SharedBlackbox(const SharedBlackbox&) = delete;
  SharedBlackbox& operator=(const SharedBlackbox&) = delete;

  void Append(const BlackboxRecord& record);
  std::vector<BlackboxRecord> Snapshot() const;
  // The newest record only. The editor's 500 ms liveness poll uses this instead
  // of Snapshot, which allocates and copies the whole ring (786 KB at the
  // default capacity) on the UI thread for a single state byte.
  std::optional<BlackboxRecord> Latest() const;
  std::uint64_t appended() const;
  std::uint64_t dropped() const;
  std::size_t capacity() const { return capacity_; }
  bool valid() const { return header_ != nullptr; }

 private:
  struct Header {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t record_size;
    std::uint32_t capacity;
    std::uint32_t reserved;
    // The size the creator mapped. Open refuses a header whose declared ring
    // would run past this (or past the view), so a forged or corrupt header
    // cannot make Snapshot/Latest read outside the section.
    std::uint64_t mapping_bytes;
    std::atomic<std::uint64_t> appended;
    std::atomic<std::uint64_t> dropped;
  };

  SharedBlackbox(HANDLE mapping, void* view, std::size_t capacity,
                 bool writable);
  static std::unique_ptr<SharedBlackbox> OpenWithAccess(
      const std::wstring& name, DWORD access, bool writable);
  void Attach(std::size_t capacity);

  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  Header* header_ = nullptr;
  BlackboxRecord* records_ = nullptr;
  std::size_t capacity_ = 0;
  bool writable_ = false;
};

#endif // _WIN32

} // namespace htmlppt
