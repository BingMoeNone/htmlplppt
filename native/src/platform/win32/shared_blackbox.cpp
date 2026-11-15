#include "platform/win32/shared_blackbox.h"

#ifdef _WIN32

#include <algorithm>
#include <cstdint>
#include <new>

namespace htmlppt {
namespace {

constexpr std::uint32_t kBlackboxMagic =
    0x424B4C42u;  // "BLKB" in little-endian memory
constexpr std::uint16_t kBlackboxVersion = 1;

static_assert(sizeof(BlackboxRecord) == 96, "shared blackbox record layout");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "shared blackbox atomics must be lock-free");

} // namespace

SharedBlackbox::SharedBlackbox(HANDLE mapping, void* view,
                               std::size_t capacity, bool writable)
    : mapping_(mapping),
      view_(view),
      capacity_(capacity),
      writable_(writable) {
  static_assert(sizeof(Header) == 32, "shared blackbox header layout");
  Attach(capacity);
}

void SharedBlackbox::Attach(std::size_t capacity) {
  header_ = static_cast<Header*>(view_);
  records_ = reinterpret_cast<BlackboxRecord*>(
      static_cast<std::uint8_t*>(view_) + sizeof(Header));
  capacity_ = capacity;
}

SharedBlackbox::~SharedBlackbox() {
  if (view_ != nullptr) UnmapViewOfFile(view_);
  if (mapping_ != nullptr) CloseHandle(mapping_);
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::Create(
    const std::wstring& name, std::size_t capacity) {
  if (capacity == 0 ||
      capacity > static_cast<std::size_t>(0xFFFFFFFFu)) {
    return nullptr;
  }
  const std::uint64_t bytes =
      sizeof(Header) + static_cast<std::uint64_t>(capacity) *
                           sizeof(BlackboxRecord);
  SetLastError(0);
  HANDLE mapping = CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      static_cast<DWORD>(bytes >> 32),
      static_cast<DWORD>(bytes & 0xFFFFFFFFu), name.c_str());
  if (mapping == nullptr) return nullptr;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mapping);
    return nullptr;
  }
  void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                             static_cast<SIZE_T>(bytes));
  if (view == nullptr) {
    CloseHandle(mapping);
    return nullptr;
  }
  auto box = std::unique_ptr<SharedBlackbox>(
      new SharedBlackbox(mapping, view, capacity, true));
  new (box->header_) Header{};
  box->header_->magic = kBlackboxMagic;
  box->header_->version = kBlackboxVersion;
  box->header_->record_size = static_cast<std::uint16_t>(sizeof(BlackboxRecord));
  box->header_->capacity = static_cast<std::uint32_t>(capacity);
  box->header_->appended.store(0, std::memory_order_relaxed);
  box->header_->dropped.store(0, std::memory_order_relaxed);
  return box;
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::Open(const std::wstring& name) {
  return OpenWithAccess(name, FILE_MAP_READ, false);
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::OpenWritable(
    const std::wstring& name) {
  return OpenWithAccess(name, FILE_MAP_ALL_ACCESS, true);
}

std::unique_ptr<SharedBlackbox> SharedBlackbox::OpenWithAccess(
    const std::wstring& name, DWORD access, bool writable) {
  HANDLE mapping = OpenFileMappingW(access, FALSE, name.c_str());
  if (mapping == nullptr) return nullptr;
  void* view = MapViewOfFile(mapping, access, 0, 0, 0);
  if (view == nullptr) {
    CloseHandle(mapping);
    return nullptr;
  }
  auto* header = static_cast<Header*>(view);
  if (header->magic != kBlackboxMagic ||
      header->version != kBlackboxVersion ||
      header->record_size != sizeof(BlackboxRecord) ||
      header->capacity == 0) {
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return nullptr;
  }
  return std::unique_ptr<SharedBlackbox>(
      new SharedBlackbox(mapping, view, header->capacity, writable));
}

void SharedBlackbox::Append(const BlackboxRecord& record) {
  if (!writable_ || header_ == nullptr || records_ == nullptr ||
      capacity_ == 0) {
    return;
  }
  const std::uint64_t index =
      header_->appended.load(std::memory_order_relaxed);
  BlackboxRecord copy = record;
  copy.sequence = index + 1;
  records_[index % capacity_] = copy;
  header_->appended.store(index + 1, std::memory_order_release);
  if (index >= capacity_) {
    header_->dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

std::vector<BlackboxRecord> SharedBlackbox::Snapshot() const {
  std::vector<BlackboxRecord> snapshot;
  if (header_ == nullptr || records_ == nullptr || capacity_ == 0) {
    return snapshot;
  }
  const std::uint64_t appended =
      header_->appended.load(std::memory_order_acquire);
  const std::size_t count =
      std::min<std::size_t>(static_cast<std::size_t>(appended), capacity_);
  const std::size_t start =
      appended > capacity_ ? static_cast<std::size_t>(appended % capacity_)
                           : 0;
  snapshot.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    snapshot.push_back(records_[(start + i) % capacity_]);
  }
  return snapshot;
}

std::uint64_t SharedBlackbox::appended() const {
  return header_ == nullptr
             ? 0
             : header_->appended.load(std::memory_order_acquire);
}

std::uint64_t SharedBlackbox::dropped() const {
  return header_ == nullptr
             ? 0
             : header_->dropped.load(std::memory_order_relaxed);
}

} // namespace htmlppt

#endif // _WIN32
