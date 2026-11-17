#include "test_main.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
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

HTMLPPT_TEST_CASE(config_editor_apply_edit_updates_all_fields_at_once) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  htmlppt::TargetEdit edit;
  edit.slide = 4;
  edit.label = "Live";
  edit.html = "page2.html";
  edit.enabled = false;
  std::string error;
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  const auto row = model.rows()[0];
  HTMLPPT_CHECK_EQ(row.slide, 4);
  HTMLPPT_CHECK_EQ(row.label, std::string{"Live"});
  HTMLPPT_CHECK_EQ(row.html, std::string{"page2.html"});
  HTMLPPT_CHECK(!row.enabled);
  HTMLPPT_CHECK(model.dirty());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_apply_edit_rejects_bad_input) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  htmlppt::TargetEdit edit;
  edit.slide = 0;
  std::string error;
  HTMLPPT_CHECK(!model.ApplyEdit(0, edit, &error));
  HTMLPPT_CHECK(!error.empty());
  edit.slide = 1;
  edit.html.clear();
  HTMLPPT_CHECK(!model.ApplyEdit(0, edit, &error));
  edit.html = "page2.html";
  HTMLPPT_CHECK(!model.ApplyEdit(5, edit, &error));
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
HTMLPPT_TEST_CASE(config_editor_save_with_backup_keeps_the_previous_file) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  const std::string first = ReadAll(path);
  // A CJK label must round-trip through the save.
  HTMLPPT_CHECK(model.SetLabel(0, std::string{"\xE5\xB1\x95\xE7\xA4\xBA"}, &error));
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(std::filesystem::exists(dir / "watcher.json.bak"));
  HTMLPPT_CHECK_EQ(ReadAll(dir / "watcher.json.bak"), first);
  HTMLPPT_CHECK(!std::filesystem::exists(dir / "watcher.json.tmp"));
  HTMLPPT_CHECK(ReadAll(path).find("\xE5\xB1\x95\xE7\xA4\xBA") != std::string::npos);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_save_with_backup_leaves_the_file_on_failure) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  const std::string before = ReadAll(path);
  // A directory in place of the temp file makes the write fail.
  const auto blocker = dir / "watcher.json.tmp";
  std::filesystem::create_directories(blocker);
  HTMLPPT_CHECK(!model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(!error.empty());
  HTMLPPT_CHECK_EQ(ReadAll(path), before);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}
HTMLPPT_TEST_CASE(config_editor_display_rows_mark_the_failing_target) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.AddTarget(1, "page2.html", &error));  // duplicate slide 1
  const auto deck = MakeDeck();
  const auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{2});
  // The resolver flags the later duplicate against its own path; the first
  // mapping stays valid.
  HTMLPPT_CHECK_EQ(rows[0].state, std::string{"resolved"});
  HTMLPPT_CHECK_EQ(rows[1].state, std::string{"error"});
  HTMLPPT_CHECK(rows[1].issue.find("already mapped") != std::string::npos);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_display_rows_keep_a_clean_config_clean) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto deck = MakeDeck();
  const auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(rows[0].state, std::string{"resolved"});
  HTMLPPT_CHECK(rows[0].issue.empty());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_display_rows_expose_enabled_and_explicit_assets) {
  const auto dir = MakeDir();
  Config config = MakeConfig(dir);
  config.active.targets[0].enabled = false;
  ConfigEditorModel model(config);
  const auto deck = MakeDeck();
  auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK(!rows[0].enabled);
  HTMLPPT_CHECK(!rows[0].assets_explicit);
  // No assets block inherited: the effective stage mode still applies.
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"file"});

  htmlppt::TargetEdit edit;
  edit.slide = 1;
  edit.html = "page1.html";
  edit.enabled = false;
  edit.assets_mode = "tree";
  std::string error;
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK(!rows[0].enabled);
  HTMLPPT_CHECK(rows[0].assets_explicit);
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"tree"});
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_status_text_reports_the_missing_file_issue) {
  htmlppt::ConfigLoadResult loaded;
  loaded.issues.push_back(htmlppt::ConfigIssue{htmlppt::IssueSeverity::Error,
                                               "deck", "deck is missing"});
  const std::string text =
      htmlppt::ConfigStatusText(loaded, {}, "watcher.json");
  HTMLPPT_CHECK_EQ(text,
                   std::string{"cannot load watcher.json: deck is missing"});
}

HTMLPPT_TEST_CASE(config_status_text_reports_the_first_error_row) {
  htmlppt::ConfigLoadResult loaded;
  loaded.config = Config{};
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].state = "resolved";
  rows[1].state = "error";
  rows[1].issue = "slide 1 is already mapped by target 0";
  const std::string text =
      htmlppt::ConfigStatusText(loaded, rows, "watcher.json");
  HTMLPPT_CHECK_EQ(
      text,
      std::string{"2 target(s) - slide 1 is already mapped by target 0"});
}

HTMLPPT_TEST_CASE(config_status_text_is_clean_for_a_clean_config) {
  htmlppt::ConfigLoadResult loaded;
  loaded.config = Config{};
  std::vector<htmlppt::DisplayRow> rows(2);
  rows[0].state = "resolved";
  rows[1].state = "disabled";
  const std::string text =
      htmlppt::ConfigStatusText(loaded, rows, "watcher.json");
  HTMLPPT_CHECK_EQ(text, std::string{"2 target(s)"});
  HTMLPPT_CHECK(text.find(" - ") == std::string::npos);
}