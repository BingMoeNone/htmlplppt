#include "test_main.h"
#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/deck_info.h"
#include "core/utf8_path.h"

using htmlppt::DeckInfoResult;
using htmlppt::IssueSeverity;

namespace {

// The basename of a UTF-8 path, so an assertion names the file without
// depending on the platform's path separator.
std::string BaseName(const std::string& utf8_path) {
  return htmlppt::PathFromUtf8(utf8_path).filename().string();
}

DeckInfoResult ParseDeckInfo(std::string_view text) {
  return htmlppt::LoadDeckInfoFromJson(text, std::filesystem::path{"deck.pptx"});
}

bool HasError(const DeckInfoResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

bool HasIssueAt(const DeckInfoResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path) return true;
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(deck_info_parses_valid_json) {
  const auto result = ParseDeckInfo(R"({
    "slides": [
      { "index": 2, "title": "Second", "name": "Slide2" },
      { "index": 1, "title": "First", "name": "Slide1" }
    ]
  })");
  HTMLPPT_CHECK(result.info.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.info->slides.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(result.info->slides[0].index, 1);
  HTMLPPT_CHECK_EQ(result.info->slides[0].title, std::string{"First"});
  HTMLPPT_CHECK_EQ(result.info->slides[1].index, 2);
  HTMLPPT_CHECK_EQ(result.info->slides[1].name, std::string{"Slide2"});
}

HTMLPPT_TEST_CASE(deck_info_requires_contiguous_indices) {
  const auto result = ParseDeckInfo(R"({
    "slides": [
      { "index": 1, "title": "One" },
      { "index": 3, "title": "Three" }
    ]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_duplicate_index) {
  const auto result = ParseDeckInfo(R"({
    "slides": [
      { "index": 1, "title": "One" },
      { "index": 1, "title": "Again" }
    ]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides[1].index"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_missing_slides) {
  const auto result = ParseDeckInfo(R"({ "version": 1 })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_empty_slides) {
  const auto result = ParseDeckInfo(R"({ "slides": [] })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_unknown_fields) {
  const auto result = ParseDeckInfo(R"({
    "slides": [{ "index": 1, "title": "One", "mystery": true }]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides[0].mystery"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_unknown_root_field) {
  const auto result = ParseDeckInfo(R"({
    "mystery": true,
    "slides": [{ "index": 1, "title": "One" }]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "mystery"));
}

HTMLPPT_TEST_CASE(deck_info_reports_parse_error) {
  const auto result = ParseDeckInfo(R"({ "slides": [)");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "$"));
}

HTMLPPT_TEST_CASE(deck_info_provider_reads_file) {
  htmlppt::test::TempDir dir;
  const auto info_path = dir.path() / "deck-info.json";
  {
    std::ofstream out(info_path, std::ios::binary);
    out << R"({ "slides": [{ "index": 1, "title": "One" }] })";
  }
  htmlppt::JsonDeckInfoProvider provider(info_path);
  const auto result = provider.Load(dir.path() / "deck.pptx");
  HTMLPPT_CHECK(result.info.has_value());
  HTMLPPT_CHECK_EQ(result.info->slides.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.info->slides[0].title, std::string{"One"});
}

HTMLPPT_TEST_CASE(deck_info_provider_reports_missing_file) {
  htmlppt::test::TempDir dir;
  htmlppt::JsonDeckInfoProvider provider(dir.path() / "missing.json");
  const auto result = provider.Load(dir.path() / "deck.pptx");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "$"));
}

HTMLPPT_TEST_CASE(deck_thumbnail_file_names_are_zero_padded) {
  const std::filesystem::path dir = std::filesystem::path{"thumbs"};
  const auto names = htmlppt::ThumbnailFileNames(3, dir);
  HTMLPPT_CHECK_EQ(names.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(BaseName(names[0]), std::string{"slide-01.bmp"});
  HTMLPPT_CHECK_EQ(BaseName(names[1]), std::string{"slide-02.bmp"});
  HTMLPPT_CHECK_EQ(BaseName(names[2]), std::string{"slide-03.bmp"});
  // Every name lives in the requested directory.
  for (const auto& name : names) {
    HTMLPPT_CHECK(htmlppt::PathFromUtf8(name).parent_path() == dir);
  }
}

HTMLPPT_TEST_CASE(deck_thumbnail_file_names_widen_a_large_deck) {
  const std::filesystem::path dir = std::filesystem::path{"thumbs"};
  const auto names = htmlppt::ThumbnailFileNames(120, dir);
  HTMLPPT_CHECK_EQ(names.size(), std::size_t{120});
  HTMLPPT_CHECK_EQ(BaseName(names[0]), std::string{"slide-001.bmp"});
  HTMLPPT_CHECK_EQ(BaseName(names[99]), std::string{"slide-100.bmp"});
  HTMLPPT_CHECK_EQ(BaseName(names[119]), std::string{"slide-120.bmp"});
}

HTMLPPT_TEST_CASE(deck_thumbnail_file_names_are_empty_without_slides) {
  const std::filesystem::path dir = std::filesystem::path{"thumbs"};
  HTMLPPT_CHECK(htmlppt::ThumbnailFileNames(0, dir).empty());
  HTMLPPT_CHECK(htmlppt::ThumbnailFileNames(-3, dir).empty());
}

HTMLPPT_TEST_CASE(deck_thumbnail_file_names_keep_a_non_ascii_directory) {
  const std::filesystem::path dir =
      htmlppt::PathFromUtf8("\xE6\xBC\x94\xE7\xA4\xBA");
  const auto names = htmlppt::ThumbnailFileNames(1, dir);
  HTMLPPT_CHECK_EQ(names.size(), std::size_t{1});
  HTMLPPT_CHECK(htmlppt::PathFromUtf8(names[0]).parent_path() == dir);
}

HTMLPPT_TEST_CASE(deck_slide_thumbnails_pair_numbers_with_paths) {
  const std::filesystem::path dir = std::filesystem::path{"thumbs"};
  const auto thumbnails = htmlppt::SlideThumbnails(2, dir);
  HTMLPPT_CHECK_EQ(thumbnails.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(thumbnails[0].number, 1);
  HTMLPPT_CHECK_EQ(thumbnails[1].number, 2);
  const auto names = htmlppt::ThumbnailFileNames(2, dir);
  HTMLPPT_CHECK_EQ(thumbnails[0].image_path, names[0]);
  HTMLPPT_CHECK_EQ(thumbnails[1].image_path, names[1]);
  HTMLPPT_CHECK(htmlppt::SlideThumbnails(0, dir).empty());
}

HTMLPPT_TEST_CASE(deck_fingerprint_is_stable_and_content_sensitive) {
  htmlppt::test::TempDir dir;
  const auto first = dir.path() / "deck.pptx";
  const auto second = dir.path() / "other.pptx";
  {
    std::ofstream out(first, std::ios::binary);
    out << "PK-one";
  }
  {
    std::ofstream out(second, std::ios::binary);
    out << "PK-two-longer";
  }
  const auto first_hash = htmlppt::ComputeDeckFingerprint(first);
  HTMLPPT_CHECK_EQ(first_hash.size(), std::size_t{16});
  HTMLPPT_CHECK_EQ(first_hash, htmlppt::ComputeDeckFingerprint(first));
  HTMLPPT_CHECK(first_hash != htmlppt::ComputeDeckFingerprint(second));
  HTMLPPT_CHECK(htmlppt::ComputeDeckFingerprint(dir.path() / "missing.pptx").empty());
}
