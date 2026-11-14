#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace htmlppt {

struct JournalRecord {
  std::string run_id;
  std::uint64_t sequence = 0;
  std::string timestamp;
  std::string event;
  std::string state;
  int slide = 0;
  std::string detail;
};

class Journal {
 public:
  virtual ~Journal() = default;
  virtual void append(const JournalRecord& record) = 0;
  virtual void flush() = 0;
  virtual std::vector<JournalRecord> read_tail(std::size_t max_records) const = 0;
};

class InMemoryJournal final : public Journal {
 public:
  void append(const JournalRecord& record) override;
  void flush() override;
  std::vector<JournalRecord> read_tail(std::size_t max_records) const override;

 private:
  std::vector<JournalRecord> records_;
  std::uint64_t next_sequence_ = 0;
};

class JsonlJournal final : public Journal {
 public:
  explicit JsonlJournal(std::filesystem::path path);

  void append(const JournalRecord& record) override;
  void flush() override;
  std::vector<JournalRecord> read_tail(std::size_t max_records) const override;

 private:
  std::filesystem::path path_;
  std::ofstream output_;
  std::uint64_t next_sequence_ = 0;
};

} // namespace htmlppt
