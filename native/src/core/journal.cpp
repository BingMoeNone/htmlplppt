#include "core/journal.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace htmlppt {
namespace {

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm parts{};
#ifdef _WIN32
  gmtime_s(&parts, &time);
#else
  gmtime_r(&time, &parts);
#endif
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

} // namespace

void InMemoryJournal::append(const JournalRecord& record) {
  auto copy = record;
  copy.sequence = next_sequence_++;
  if (copy.timestamp.empty()) {
    copy.timestamp = utc_timestamp();
  }
  records_.push_back(std::move(copy));
}

void InMemoryJournal::flush() {}

std::vector<JournalRecord> InMemoryJournal::read_tail(
    std::size_t max_records) const {
  if (max_records >= records_.size()) {
    return records_;
  }
  return std::vector<JournalRecord>(records_.end() -
                                        static_cast<std::ptrdiff_t>(max_records),
                                    records_.end());
}

JsonlJournal::JsonlJournal(std::filesystem::path path)
    : path_(std::move(path)) {
  if (!path_.parent_path().empty()) {
    std::filesystem::create_directories(path_.parent_path());
  }
  output_.open(path_, std::ios::app);
  if (!output_.is_open()) {
    throw std::runtime_error("failed to open journal file: " + path_.string());
  }
}

void JsonlJournal::append(const JournalRecord& record) {
  auto copy = record;
  copy.sequence = next_sequence_++;
  if (copy.timestamp.empty()) {
    copy.timestamp = utc_timestamp();
  }

  nlohmann::json line{
      {"run_id", copy.run_id},
      {"sequence", copy.sequence},
      {"timestamp", copy.timestamp},
      {"event", copy.event},
      {"state", copy.state},
      {"slide", copy.slide},
      {"detail", copy.detail},
  };
  output_ << line.dump() << '\n';
}

void JsonlJournal::flush() {
  output_.flush();
}

std::vector<JournalRecord> JsonlJournal::read_tail(
    std::size_t max_records) const {
  std::ifstream input(path_);
  std::vector<JournalRecord> records;
  std::string text;
  while (std::getline(input, text)) {
    const auto line = nlohmann::json::parse(text, nullptr, false);
    if (line.is_discarded() || !line.is_object()) {
      continue;
    }
    JournalRecord record;
    try {
      record.run_id = line.value("run_id", std::string{});
      record.sequence = line.value("sequence", std::uint64_t{0});
      record.timestamp = line.value("timestamp", std::string{});
      record.event = line.value("event", std::string{});
      record.state = line.value("state", std::string{});
      record.slide = line.value("slide", 0);
      record.detail = line.value("detail", std::string{});
    } catch (const nlohmann::json::exception&) {
      continue;
    }
    records.push_back(std::move(record));
  }

  if (max_records < records.size()) {
    records.erase(records.begin(),
                  records.end() - static_cast<std::ptrdiff_t>(max_records));
  }
  return records;
}

} // namespace htmlppt
