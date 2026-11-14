#include "test_main.h"
#include "test_utils.h"

#include <cstdint>
#include <fstream>
#include <string>

#include "core/journal.h"

using htmlppt::InMemoryJournal;
using htmlppt::JournalRecord;
using htmlppt::JsonlJournal;
using htmlppt::test::TempDir;

HTMLPPT_TEST_CASE(in_memory_journal_assigns_sequences) {
  InMemoryJournal journal;
  JournalRecord record;
  record.run_id = "run-1";
  record.event = "warm";
  record.state = "Warming";
  journal.append(record);

  record.event = "monitor";
  record.state = "Monitoring";
  journal.append(record);

  const auto tail = journal.read_tail(10);
  HTMLPPT_CHECK_EQ(tail.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(tail[0].sequence, std::uint64_t{0});
  HTMLPPT_CHECK_EQ(tail[1].sequence, std::uint64_t{1});
  HTMLPPT_CHECK_EQ(tail[1].event, std::string{"monitor"});
}

HTMLPPT_TEST_CASE(jsonl_journal_round_trips) {
  TempDir directory;
  const auto path = directory.path() / "journal.jsonl";
  {
    JsonlJournal journal(path);
    JournalRecord record;
    record.run_id = "run-2";
    record.event = "cover";
    record.state = "CoverIn";
    journal.append(record);
    record.event = "done";
    journal.append(record);
    journal.flush();
  }

  JsonlJournal reader(path);
  const auto tail = reader.read_tail(1);
  HTMLPPT_CHECK_EQ(tail.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(tail[0].event, std::string{"done"});
  HTMLPPT_CHECK_EQ(tail[0].run_id, std::string{"run-2"});
}

HTMLPPT_TEST_CASE(jsonl_journal_skips_malformed_lines) {
  TempDir directory;
  const auto path = directory.path() / "journal.jsonl";
  {
    std::ofstream output(path);
    output << "not-json\n";
    output << R"({"run_id":"run-3","sequence":1,"timestamp":"t","event":"ok","state":"Live","slide":0,"detail":""})"
           << "\n";
  }

  JsonlJournal journal(path);
  const auto tail = journal.read_tail(10);
  HTMLPPT_CHECK_EQ(tail.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(tail[0].event, std::string{"ok"});
}
