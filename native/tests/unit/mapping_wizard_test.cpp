#include "test_main.h"

#include <string>
#include <utility>
#include <vector>

#include "core/mapping_wizard.h"

using htmlppt::PairingMode;
using htmlppt::ProposePairs;
using htmlppt::SuggestHtmlRoot;
using htmlppt::WizardPair;
using htmlppt::WizardSlide;

namespace {

WizardSlide Slide(int number, std::string title) {
  WizardSlide slide;
  slide.number = number;
  slide.title = std::move(title);
  return slide;
}

std::vector<WizardPair> Propose(PairingMode mode,
                                std::vector<WizardSlide> slides,
                                std::vector<std::string> files) {
  return ProposePairs(slides, files, mode);
}

} // namespace

HTMLPPT_TEST_CASE(mapping_wizard_by_file_name_uses_the_trailing_stem_number) {
  const std::vector<WizardSlide> slides = {Slide(1, "One"), Slide(2, "Two"),
                                           Slide(3, "Three")};
  const std::vector<std::string> files = {"cover.html", "slide-1.html",
                                          "page-02.HTML", "extra.txt"};
  const auto pairs = Propose(PairingMode::ByFileName, slides, files);

  HTMLPPT_CHECK_EQ(pairs.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(pairs[0].slide, 1);
  HTMLPPT_CHECK_EQ(pairs[0].title, std::string{"One"});
  HTMLPPT_CHECK_EQ(pairs[0].html, std::string{"slide-1.html"});
  HTMLPPT_CHECK(pairs[0].included);
  HTMLPPT_CHECK_EQ(pairs[1].slide, 2);
  HTMLPPT_CHECK_EQ(pairs[1].title, std::string{"Two"});
  HTMLPPT_CHECK_EQ(pairs[1].html, std::string{"page-02.HTML"});
}

HTMLPPT_TEST_CASE(mapping_wizard_by_file_name_keeps_the_first_file_per_slide) {
  const std::vector<WizardSlide> slides = {Slide(1, "One"), Slide(2, "Two")};
  const std::vector<std::string> files = {
      "page-1.html", "slide-1.html", "slide-2.html", "slide-9.html"};
  const auto pairs = Propose(PairingMode::ByFileName, slides, files);

  HTMLPPT_CHECK_EQ(pairs.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(pairs[0].html, std::string{"page-1.html"});
  HTMLPPT_CHECK_EQ(pairs[1].html, std::string{"slide-2.html"});
}

HTMLPPT_TEST_CASE(mapping_wizard_by_file_name_ignores_non_html_and_unmatched) {
  const std::vector<WizardSlide> slides = {Slide(4, "Four")};
  const std::vector<std::string> files = {"notes.txt", "page.html",
                                          "slide-4.html.bak", "page-4x.html",
                                          "4", "slide-4.HTM"};
  const auto pairs = Propose(PairingMode::ByFileName, slides, files);

  HTMLPPT_CHECK_EQ(pairs.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(pairs[0].slide, 4);
  HTMLPPT_CHECK_EQ(pairs[0].html, std::string{"slide-4.HTM"});
}

HTMLPPT_TEST_CASE(mapping_wizard_by_file_name_pairs_nested_paths) {
  const std::vector<WizardSlide> slides = {Slide(2, "Two")};
  const std::vector<std::string> files = {"pages/a/slide-02.html"};
  const auto pairs = Propose(PairingMode::ByFileName, slides, files);

  HTMLPPT_CHECK_EQ(pairs.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(pairs[0].slide, 2);
  HTMLPPT_CHECK_EQ(pairs[0].html, std::string{"pages/a/slide-02.html"});
}

HTMLPPT_TEST_CASE(mapping_wizard_by_file_order_zips_sorted_html_files) {
  const std::vector<WizardSlide> slides = {Slide(1, "A"), Slide(2, "B"),
                                           Slide(3, "C")};
  const std::vector<std::string> files = {"b.html", "a.html", "c.htm",
                                          "notes.txt"};
  const auto pairs = Propose(PairingMode::ByFileOrder, slides, files);

  HTMLPPT_CHECK_EQ(pairs.size(), std::size_t{3});
  HTMLPPT_CHECK_EQ(pairs[0].slide, 1);
  HTMLPPT_CHECK_EQ(pairs[0].title, std::string{"A"});
  HTMLPPT_CHECK_EQ(pairs[0].html, std::string{"a.html"});
  HTMLPPT_CHECK_EQ(pairs[1].slide, 2);
  HTMLPPT_CHECK_EQ(pairs[1].html, std::string{"b.html"});
  HTMLPPT_CHECK_EQ(pairs[2].slide, 3);
  HTMLPPT_CHECK_EQ(pairs[2].html, std::string{"c.htm"});
}

HTMLPPT_TEST_CASE(mapping_wizard_by_file_order_ignores_leftovers) {
  const std::vector<WizardSlide> slides = {Slide(1, "A"), Slide(2, "B")};
  const auto pairs = Propose(PairingMode::ByFileOrder, slides,
                             {"a.html", "b.html", "c.html"});
  HTMLPPT_CHECK_EQ(pairs.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(pairs[1].html, std::string{"b.html"});

  const auto fewer = Propose(PairingMode::ByFileOrder, slides, {"a.html"});
  HTMLPPT_CHECK_EQ(fewer.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(fewer[0].html, std::string{"a.html"});
}

HTMLPPT_TEST_CASE(mapping_wizard_suggest_html_root_returns_common_directory) {
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({"pages/a.html", "pages/b.html"}),
                   std::string{"pages"});
  HTMLPPT_CHECK_EQ(
      SuggestHtmlRoot({"pages/sub/a.html", "pages/sub/b.html"}),
      std::string{"pages/sub"});
  HTMLPPT_CHECK_EQ(
      SuggestHtmlRoot({"pages/a.html", "pages/sub/b.html"}),
      std::string{"pages"});
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({"slides/a.html"}),
                   std::string{"slides"});
  // Native Windows separators are accepted too: the wizard scans the file
  // system with std::filesystem and reports UTF-8 paths.
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({"pages\\a.html", "pages\\b.html"}),
                   std::string{"pages"});
}

HTMLPPT_TEST_CASE(mapping_wizard_suggest_html_root_handles_no_common_root) {
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({"pages/a.html", "other/b.html"}),
                   std::string{});
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({"a.html", "b.html"}), std::string{});
}

HTMLPPT_TEST_CASE(mapping_wizard_suggest_html_root_handles_empty_input) {
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({}), std::string{});
  HTMLPPT_CHECK_EQ(SuggestHtmlRoot({"a.html"}), std::string{});
}
