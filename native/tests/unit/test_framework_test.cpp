#include "test_main.h"

#include <cstddef>
#include <cstring>
#include <new>
#include <vector>

namespace {

// Fills the storage on deallocation so a dangling reference is observable on
// every platform instead of relying on freed memory keeping its old bytes.
template <typename T>
struct PoisonAlloc {
  using value_type = T;

  PoisonAlloc() = default;
  template <typename U>
  PoisonAlloc(const PoisonAlloc<U>&) {}

  T* allocate(std::size_t count) {
    return static_cast<T*>(::operator new(count * sizeof(T)));
  }
  void deallocate(T* storage, std::size_t count) {
    std::memset(storage, 0xFF, count * sizeof(T));
    ::operator delete(storage);
  }

  template <typename U>
  bool operator==(const PoisonAlloc<U>&) const {
    return true;
  }
  template <typename U>
  bool operator!=(const PoisonAlloc<U>&) const {
    return false;
  }
};

std::vector<int, PoisonAlloc<int>> PoisonedVector() {
  return std::vector<int, PoisonAlloc<int>>{1, 2, 3};
}

} // namespace

// Regression for the CHECK_EQ dangling-reference pattern: the macro used to bind
// `const auto&` to `rows()[i]`, which is a reference into a temporary vector.
// The vector dies at the semicolon and the comparison then reads freed storage.
// A poisoning allocator makes that read deterministic, so this test fails on
// MSVC too instead of passing by luck.
HTMLPPT_TEST_CASE(test_framework_check_eq_copies_before_the_temporary_dies) {
  HTMLPPT_CHECK_EQ(PoisonedVector()[0], 1);
}
