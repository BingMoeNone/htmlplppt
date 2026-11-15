#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/blackbox.h"

namespace htmlppt {

#ifdef _WIN32

class SharedBlackbox {
 public:
  static std::unique_ptr<SharedBlackbox> Create(const std::wstring& name,
                                                std::size_t capacity);
  static std::unique_ptr<SharedBlackbox> Open(const std::wstring& name);
  ~SharedBlackbox();

  SharedBlackbox(const SharedBlackbox&) = delete;
  SharedBlackbox& operator=(const SharedBlackbox&) = delete;

  void Append(const BlackboxRecord& record);
  std::vector<BlackboxRecord> Snapshot() const;
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
    std::atomic<std::uint64_t> appended;
    std::atomic<std::uint64_t> dropped;
  };

  SharedBlackbox(HANDLE mapping, void* view, std::size_t capacity);
  void Attach(std::size_t capacity);

  HANDLE mapping_ = nullptr;
  void* view_ = nullptr;
  Header* header_ = nullptr;
  BlackboxRecord* records_ = nullptr;
  std::size_t capacity_ = 0;
};

#endif // _WIN32

} // namespace htmlppt
