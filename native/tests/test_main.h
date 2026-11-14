#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace htmlppt::test {

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct TestCase {
  std::string name;
  std::function<void()> function;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, std::function<void()> function) {
    registry().push_back({std::move(name), std::move(function)});
  }
};

inline int run_all() {
  int failed = 0;
  for (const auto& test : registry()) {
    try {
      test.function();
      std::cout << "[PASS] " << test.name << "\n";
    } catch (const Failure& error) {
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << "\n";
      ++failed;
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test.name
                << ": unexpected exception: " << error.what() << "\n";
      ++failed;
    } catch (...) {
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
      ++failed;
    }
  }
  std::cout << (failed == 0 ? "ALL TESTS PASSED" : "TESTS FAILED") << "\n";
  return failed == 0 ? 0 : 1;
}

} // namespace htmlppt::test

#define HTMLPPT_TEST_CASE(name)                                      \
  static void name();                                                \
  static const ::htmlppt::test::Registrar name##_registrar(#name, name); \
  static void name()

#define HTMLPPT_CHECK(condition)                                      \
  do {                                                                \
    if (!(condition)) {                                               \
      std::ostringstream message;                                     \
      message << __FILE__ << ":" << __LINE__                          \
              << ": CHECK(" #condition ") failed";                    \
      throw ::htmlppt::test::Failure(message.str());                  \
    }                                                                 \
  } while (0)

#define HTMLPPT_CHECK_EQ(lhs, rhs)                                    \
  do {                                                                \
    const auto& left_value = (lhs);                                   \
    const auto& right_value = (rhs);                                  \
    if (!(left_value == right_value)) {                               \
      std::ostringstream message;                                     \
      message << __FILE__ << ":" << __LINE__                          \
              << ": CHECK_EQ(" #lhs ", " #rhs ") failed";             \
      throw ::htmlppt::test::Failure(message.str());                  \
    }                                                                 \
  } while (0)
