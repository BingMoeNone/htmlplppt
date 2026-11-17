#include "test_main.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/config_editor.h"
#include "core/deck_info.h"
#include "core/resolve.h"

using htmlppt::Config;
using htmlppt::ConfigEditorModel;
using htmlppt::DeckInfo;
using htmlppt::DeckSlideInfo;
using htmlppt::ResolveOptions;
using htmlppt::TargetConfig;

namespace {

std::filesystem::path MakeDir() {
  const auto dir = std::filesystem::temp_directory_path() / "htmlppt-editor-test";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  std::ofstream(dir / "page1.html") << "<html></html>";
  std::ofstream(dir / "page2.html") << "<html></html>";
  return dir;
}

Config MakeConfig(const std::filesystem::path& dir) {
  Config config;
  config.version = 1;
  config.source_path = dir / "watcher.json";
  config.active.deck = "deck.pptx";
  config.active.stage.mode = "file";
  TargetConfig target;
  target.slide = 1;
  target.html = "page1.html";
  target.label = "One";
  config.active.targets.push_back(target);
  return config;
}

DeckInfo MakeDeck() {
  DeckInfo deck;
  for (int index = 1; index <= 5; ++index) {
    DeckSlideInfo slide;
    slide.index = index;
    slide.title = "Slide " + std::to_string(index);
    deck.slides.push_back(slide);
  }
  return deck;
}

} // namespace

HTMLPPT_TEST_CASE(config_editor_rows_reflect_the_effective_config) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  HTMLPPT_CHECK(!model.dirty());
  const auto rows = model.rows();
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(rows[0].slide, 1);
  HTMLPPT_CHECK_EQ(rows[0].html, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"file"});
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

HTMLPPT_TEST_CASE(config_editor_edits_marks_dirty_and_rejects_bad_input) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 3, &error));
  HTMLPPT_CHECK(model.dirty());
  HTMLPPT_CHECK_EQ(model.rows()[0].slide, 3);

  HTMLPPT_CHECK(!model.SetSlide(0, 0, &error));
  HTMLPPT_CHECK(!error.empty());
  HTMLPPT_CHECK(!model.SetHtml(0, "", &error));
  HTMLPPT_CHECK(!model.SetSlide(9, 2, &error));

  HTMLPPT_CHECK_EQ(model.target_count(), std::size_t{1});
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_add_remove_targets) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.AddTarget(5, "page2.html", &error));
  HTMLPPT_CHECK_EQ(model.target_count(), std::size_t{2});
  HTMLPPT_CHECK_EQ(model.rows()[1].slide, 5);

  HTMLPPT_CHECK(model.RemoveTarget(0, &error));
  HTMLPPT_CHECK_EQ(model.target_count(), std::size_t{1});
  HTMLPPT_CHECK_EQ(model.rows()[0].html, std::string{"page2.html"});
  HTMLPPT_CHECK(!model.RemoveTarget(4, &error));
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_validate_reports_duplicate_slides) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.AddTarget(1, "page2.html", &error));  // duplicate slide 1
  const auto deck = MakeDeck();
  const auto issues = model.Validate(&deck, ResolveOptions{});
  bool saw_duplicate = false;
  for (const auto& issue : issues) {
    if (issue.message.find("duplicate") != std::string::npos ||
        issue.path.find("targets[1]") != std::string::npos) {
      saw_duplicate = true;
    }
  }
  HTMLPPT_CHECK(saw_duplicate);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_save_is_atomic_and_clears_dirty) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 4, &error));

  const auto path = dir / "watcher.json";
  HTMLPPT_CHECK(model.Save(path, &error));
  HTMLPPT_CHECK(std::filesystem::is_regular_file(path));
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "watcher.json.tmp"));
  HTMLPPT_CHECK(!model.dirty());

  // The saved file parses back through the production loader.
  std::ifstream input(path, std::ios::binary);
  const std::string saved_text((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
  const auto saved = htmlppt::LoadConfigFromString(saved_text, path, {});
  HTMLPPT_CHECK(saved.config.has_value());
  HTMLPPT_CHECK_EQ(saved.config->active.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(saved.config->active.targets[0].slide.value_or(0), 4);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
