#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace htmlppt::test {

class TempDir {
 public:
  TempDir() {
    static std::atomic<unsigned long long> counter{0};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("htmlppt-test-" + std::to_string(stamp) + "-" +
             std::to_string(counter++));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

} // namespace htmlppt::test
