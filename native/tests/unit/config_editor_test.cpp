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
  // MakeConfig points at deck.pptx; the resolver reports a global error when it
  // is missing, which HasBlockingError must see.
  std::ofstream(dir / "deck.pptx") << "";
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

HTMLPPT_TEST_CASE(config_editor_display_rows_resolve_a_match_target_slide) {
  const auto dir = MakeDir();
  Config config = MakeConfig(dir);
  // A match target has no literal slide; the resolver owns the number.
  config.active.targets[0].slide.reset();
  htmlppt::MatchConfig match;
  match.slide_title = "Slide 4";
  match.mode = "exact";
  config.active.targets[0].match = match;
  ConfigEditorModel model(config);
  const auto deck = MakeDeck();
  const auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(rows[0].state, std::string{"resolved"});
  // Showing 0 here made every Apply rewrite the target as slide 1.
  HTMLPPT_CHECK_EQ(rows[0].slide, 4);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_apply_edit_keeps_a_match_target_on_match) {
  const auto dir = MakeDir();
  Config config = MakeConfig(dir);
  config.active.targets[0].slide.reset();
  htmlppt::MatchConfig match;
  match.slide_title = "Slide 4";
  match.mode = "exact";
  config.active.targets[0].match = match;
  ConfigEditorModel model(config);

  htmlppt::TargetEdit edit;
  // A match target needs no numeric slide; the UI may still show 0 before the
  // deck is resolved, and that must not reject an unrelated edit.
  edit.slide = 0;
  edit.label = "Live";
  edit.html = "page2.html";
  edit.enabled = false;
  std::string error;
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  const auto& target = model.config().active.targets[0];
  HTMLPPT_CHECK(target.match.has_value());
  HTMLPPT_CHECK_EQ(target.match->slide_title, std::string{"Slide 4"});
  HTMLPPT_CHECK(!target.slide.has_value());
  HTMLPPT_CHECK_EQ(target.label, std::string{"Live"});
  HTMLPPT_CHECK_EQ(target.html, std::string{"page2.html"});
  HTMLPPT_CHECK(!target.enabled);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(
    config_editor_reload_from_disk_resets_history_and_reads_new_content) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  // Give the model a history and a dirty state first: an external reload must
  // not leave an undo path that crosses it.
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(model.SetSlide(0, 4, &error));
  HTMLPPT_CHECK(model.CanUndo());
  HTMLPPT_CHECK(model.dirty());

  // Another program rewrites the file.
  Config external = MakeConfig(dir);
  external.active.targets[0].slide = 7;
  external.active.targets[0].html = "page2.html";
  external.active.targets[0].label = "External";
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      << htmlppt::ConfigToJson(external);

  HTMLPPT_CHECK(model.ReloadFromDisk(path, &error));
  HTMLPPT_CHECK(error.empty());
  const auto rows = model.rows();
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(rows[0].slide, 7);
  HTMLPPT_CHECK_EQ(rows[0].html, std::string{"page2.html"});
  HTMLPPT_CHECK_EQ(rows[0].label, std::string{"External"});
  // The reloaded file is the new clean baseline: nothing before it is undoable
  // and the model is not dirty.
  HTMLPPT_CHECK(!model.dirty());
  HTMLPPT_CHECK(!model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());
  HTMLPPT_CHECK_EQ(model.config().source_path, path);
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_reload_from_disk_keeps_the_model_on_a_bad_file) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(model.SetSlide(0, 6, &error));

  // A corrupt external write must not throw the user's edits away: the load
  // fails, the model is untouched and the error names the file.
  std::ofstream(path, std::ios::binary | std::ios::trunc) << "{ not json";
  HTMLPPT_CHECK(!model.ReloadFromDisk(path, &error));
  HTMLPPT_CHECK(!error.empty());
  HTMLPPT_CHECK(model.dirty());
  HTMLPPT_CHECK_EQ(model.rows()[0].slide, 6);
  HTMLPPT_CHECK(model.CanUndo());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_detects_multi_profile_configs) {
  const auto dir = MakeDir();
  // The editor writes ConfigToJson, which flattens to the effective profile and
  // drops `profiles`/`activeProfile`; it must know which configs it cannot save.
  ConfigEditorModel single(MakeConfig(dir));
  HTMLPPT_CHECK(!single.HasProfiles());

  Config multi = MakeConfig(dir);
  multi.profiles.emplace("demo", multi.active);
  ConfigEditorModel multi_model(multi);
  HTMLPPT_CHECK(multi_model.HasProfiles());

  // An activeProfile selector alone also means the file is profile-based.
  Config selected = MakeConfig(dir);
  selected.active_profile = "demo";
  ConfigEditorModel selected_model(selected);
  HTMLPPT_CHECK(selected_model.HasProfiles());
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

HTMLPPT_TEST_CASE(config_editor_save_keeps_an_inherited_assets_block_absent) {
  const auto dir = MakeDir();
  // The base config's target has no assets block: it inherits stage.mode.
  ConfigEditorModel model(MakeConfig(dir));
  const auto path = dir / "watcher.json";
  std::string error;
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  // Writing a block for an inheriting target would turn it explicit on reload.
  HTMLPPT_CHECK(ReadAll(path).find("\"assets\"") == std::string::npos);
  const auto reloaded = htmlppt::LoadConfigFile(path);
  HTMLPPT_CHECK(reloaded.config.has_value());
  HTMLPPT_CHECK(!reloaded.config->active.targets[0].assets_set);

  // A target that owns its block must keep it explicit, with the same mode.
  Config explicit_config = MakeConfig(dir);
  explicit_config.active.targets[0].assets.mode = "tree";
  explicit_config.active.targets[0].assets_set = true;
  ConfigEditorModel explicit_model(explicit_config);
  HTMLPPT_CHECK(explicit_model.SaveWithBackup(path, &error));
  HTMLPPT_CHECK(ReadAll(path).find("\"assets\"") != std::string::npos);
  const auto explicit_reloaded = htmlppt::LoadConfigFile(path);
  HTMLPPT_CHECK(explicit_reloaded.config.has_value());
  HTMLPPT_CHECK(explicit_reloaded.config->active.targets[0].assets_set);
  HTMLPPT_CHECK_EQ(explicit_reloaded.config->active.targets[0].assets.mode,
                   std::string{"tree"});
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

HTMLPPT_TEST_CASE(config_editor_blocks_save_while_a_row_has_an_error) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  const auto deck = MakeDeck();
  std::string error;
  HTMLPPT_CHECK(!model.HasBlockingError(&deck, htmlppt::ResolveOptions{}));
  HTMLPPT_CHECK(model.AddTarget(1, "page2.html", &error));  // duplicate slide 1
  HTMLPPT_CHECK(model.HasBlockingError(&deck, htmlppt::ResolveOptions{}));
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_does_not_block_on_a_clean_or_disabled_row) {
  const auto dir = MakeDir();
  Config config = MakeConfig(dir);
  // A disabled target is a deliberate choice, not a validation error.
  config.active.targets[0].enabled = false;
  ConfigEditorModel model(config);
  const auto deck = MakeDeck();
  HTMLPPT_CHECK(!model.HasBlockingError(&deck, htmlppt::ResolveOptions{}));
  const auto rows = model.DisplayRows(&deck, htmlppt::ResolveOptions{});
  HTMLPPT_CHECK_EQ(rows[0].state, std::string{"disabled"});
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_inherit_clears_an_explicit_assets_block) {
  const auto dir = MakeDir();
  Config config = MakeConfig(dir);
  // The target owns a block whose mode differs from the stage mode.
  config.active.stage.mode = "file";
  config.active.targets[0].assets.mode = "tree";
  config.active.targets[0].assets_set = true;
  ConfigEditorModel model(config);
  const auto deck = MakeDeck();
  auto rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK(rows[0].assets_explicit);
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"tree"});

  htmlppt::TargetEdit edit;
  edit.slide = 1;
  edit.html = "page1.html";
  std::string error;
  // An empty mode still means "leave the target's own block untouched".
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK(rows[0].assets_explicit);
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"tree"});

  // "inherit" is a defined value: the pane's choice to use the stage mode
  // clears the target's block instead of pinning it.
  edit.assets_mode = "inherit";
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK(!rows[0].assets_explicit);
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"file"});

  // Any other mode makes the target own an explicit block again.
  edit.assets_mode = "direct";
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  rows = model.DisplayRows(&deck, ResolveOptions{});
  HTMLPPT_CHECK(rows[0].assets_explicit);
  HTMLPPT_CHECK_EQ(rows[0].assets_mode, std::string{"direct"});
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_blocks_save_for_a_global_error) {
  const auto dir = MakeDir();
  // The deck error is on path "deck", not on a target row: an empty deck must
  // still block a save.
  Config cleared = MakeConfig(dir);
  cleared.active.deck.clear();
  ConfigEditorModel cleared_model(cleared);
  HTMLPPT_CHECK(cleared_model.HasBlockingError(nullptr, htmlppt::ResolveOptions{}));

  // A deck path that does not exist is the same class of global error.
  std::error_code fs_error;
  std::filesystem::remove(dir / "deck.pptx", fs_error);
  ConfigEditorModel missing_model(MakeConfig(dir));
  HTMLPPT_CHECK(missing_model.HasBlockingError(nullptr, htmlppt::ResolveOptions{}));
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_undo_restores_the_previous_state) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  HTMLPPT_CHECK(!model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());
  // Nothing was mutated, so there is nothing to undo or redo.
  HTMLPPT_CHECK(!model.Undo());
  HTMLPPT_CHECK(!model.Redo());
  HTMLPPT_CHECK(!model.dirty());

  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 4, &error));
  HTMLPPT_CHECK(model.dirty());
  HTMLPPT_CHECK(model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());

  HTMLPPT_CHECK(model.Undo());
  HTMLPPT_CHECK_EQ(model.rows()[0].slide, 1);
  // Undoing the only edit returns to the loaded baseline, so dirty flips back.
  HTMLPPT_CHECK(!model.dirty());
  HTMLPPT_CHECK(!model.CanUndo());
  HTMLPPT_CHECK(model.CanRedo());

  HTMLPPT_CHECK(model.Redo());
  HTMLPPT_CHECK_EQ(model.rows()[0].slide, 4);
  HTMLPPT_CHECK(model.dirty());
  HTMLPPT_CHECK(model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_undo_past_the_start_and_redo_past_the_end_are_noops) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 4, &error));

  HTMLPPT_CHECK(model.Undo());
  const std::string baseline = model.ToJson();
  HTMLPPT_CHECK(!model.Undo());
  HTMLPPT_CHECK_EQ(model.ToJson(), baseline);

  HTMLPPT_CHECK(model.Redo());
  const std::string edited = model.ToJson();
  HTMLPPT_CHECK(!model.Redo());
  // A no-op redo must not disturb the model or the history either.
  HTMLPPT_CHECK_EQ(model.ToJson(), edited);
  HTMLPPT_CHECK(model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_undo_covers_every_mutating_method) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 2, &error));
  HTMLPPT_CHECK(model.SetHtml(0, "page2.html", &error));
  HTMLPPT_CHECK(model.SetLabel(0, "Live", &error));
  HTMLPPT_CHECK(model.SetEnabled(0, false, &error));
  htmlppt::TargetEdit edit;
  edit.slide = 3;
  edit.label = "Edit";
  edit.html = "page1.html";
  edit.enabled = true;
  HTMLPPT_CHECK(model.ApplyEdit(0, edit, &error));
  HTMLPPT_CHECK(model.AddTarget(5, "page2.html", &error));
  HTMLPPT_CHECK(model.RemoveTarget(0, &error));

  int steps = 0;
  while (model.Undo()) ++steps;
  // One snapshot per successful mutation: seven in total.
  HTMLPPT_CHECK_EQ(steps, 7);
  const auto rows = model.rows();
  HTMLPPT_CHECK_EQ(rows.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(rows[0].slide, 1);
  HTMLPPT_CHECK_EQ(rows[0].html, std::string{"page1.html"});
  HTMLPPT_CHECK_EQ(rows[0].label, std::string{"One"});
  HTMLPPT_CHECK(rows[0].enabled);
  HTMLPPT_CHECK(!model.dirty());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_failed_mutations_are_not_recorded) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(!model.SetSlide(0, 0, &error));
  HTMLPPT_CHECK(!model.SetHtml(0, "", &error));
  HTMLPPT_CHECK(!model.SetLabel(9, "nope", &error));
  HTMLPPT_CHECK(!model.SetEnabled(9, false, &error));
  htmlppt::TargetEdit edit;
  edit.slide = 0;
  edit.html = "page1.html";
  HTMLPPT_CHECK(!model.ApplyEdit(0, edit, &error));
  HTMLPPT_CHECK(!model.AddTarget(0, "page1.html", &error));
  HTMLPPT_CHECK(!model.RemoveTarget(9, &error));
  HTMLPPT_CHECK(!model.CanUndo());
  HTMLPPT_CHECK(!model.dirty());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_a_new_edit_clears_the_redo_stack) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 4, &error));
  HTMLPPT_CHECK(model.Undo());
  HTMLPPT_CHECK(model.CanRedo());
  HTMLPPT_CHECK(model.SetHtml(0, "page2.html", &error));
  // Branching off the undone state must discard the abandoned redo path.
  HTMLPPT_CHECK(!model.CanRedo());
  HTMLPPT_CHECK(!model.Redo());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_undo_history_is_capped_at_fifty) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  // Sixty successful edits; only the newest fifty snapshots may survive.
  for (int slide = 2; slide <= 61; ++slide) {
    HTMLPPT_CHECK(model.SetSlide(0, slide, &error));
  }
  int steps = 0;
  while (model.Undo()) ++steps;
  HTMLPPT_CHECK_EQ(steps, 50);
  // The oldest ten snapshots were dropped, so undo stops at the state after
  // the tenth edit (slide 11), not at the loaded slide 1.
  HTMLPPT_CHECK_EQ(model.rows()[0].slide, 11);
  HTMLPPT_CHECK(!model.CanUndo());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_save_clears_the_history_and_rebases_dirty) {
  const auto dir = MakeDir();
  ConfigEditorModel model(MakeConfig(dir));
  std::string error;
  HTMLPPT_CHECK(model.SetSlide(0, 3, &error));
  HTMLPPT_CHECK(model.Undo());  // leaves a redo entry
  HTMLPPT_CHECK(model.CanRedo());

  const auto path = dir / "watcher.json";
  HTMLPPT_CHECK(model.SaveWithBackup(path, &error));
  // A successful save is the new baseline: no history crosses it.
  HTMLPPT_CHECK(!model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());
  HTMLPPT_CHECK(!model.dirty());

  // An edit made after the save undoes back to the saved state, which is clean
  // relative to the new baseline rather than the original load.
  HTMLPPT_CHECK(model.SetLabel(0, "Saved", &error));
  HTMLPPT_CHECK(model.Undo());
  HTMLPPT_CHECK(!model.dirty());
  HTMLPPT_CHECK_EQ(model.rows()[0].label, std::string{"One"});
  HTMLPPT_CHECK(!model.CanUndo());

  // Save() is a save too: it must clear the history the same way.
  HTMLPPT_CHECK(model.SetSlide(0, 5, &error));
  HTMLPPT_CHECK(model.SetSlide(0, 2, &error));
  HTMLPPT_CHECK(model.Save(path, &error));
  HTMLPPT_CHECK(!model.CanUndo());
  HTMLPPT_CHECK(!model.CanRedo());
  HTMLPPT_CHECK(!model.dirty());
  std::error_code fs_error;
  std::filesystem::remove_all(dir, fs_error);
}

HTMLPPT_TEST_CASE(config_editor_undo_preserves_profiles_and_the_config_path) {
  const auto dir = MakeDir();
  // ConfigToJson writes only the effective profile. An undo must not silently
  // drop the profile map, which would make a read-only config look saveable and
  // lose the other profiles on the next save.
  Config multi = MakeConfig(dir);
  multi.profiles.emplace("demo", multi.active);
  multi.active_profile = "demo";
  ConfigEditorModel model(multi);
  HTMLPPT_CHECK(model.HasProfiles());
  std::string error;
  HTMLPPT_CHECK(model.SetLabel(0, "Edited", &error));
  HTMLPPT_CHECK(model.Undo());
  HTMLPPT_CHECK(model.HasProfiles());
  HTMLPPT_CHECK_EQ(model.config().active_profile, std::string{"demo"});
  HTMLPPT_CHECK_EQ(model.config().source_path, dir / "watcher.json");
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