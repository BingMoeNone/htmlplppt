#include "core/config_editor.h"

#include <fstream>
#include <utility>

#include "core/utf8_path.h"

namespace htmlppt {
namespace {

bool CheckIndex(std::size_t index, std::size_t count, std::string* error) {
  if (index < count) return true;
  if (error != nullptr) {
    *error = "target index out of range: " + std::to_string(index) +
             " of " + std::to_string(count);
  }
  return false;
}

// True when two serialized configs describe the same configuration. A plain
// string compare is not enough because the loader fills defaults that the
// serializer omits (a target's inherited onFailure, say): a snapshot restored
// through the loader is then textually different from the JSON it came from but
// means exactly the same thing. Comparing both sides after one loader pass
// keeps dirty() about the configuration, not that asymmetry.
bool JsonIsEquivalent(const std::string& lhs, const std::string& rhs,
                      const std::filesystem::path& source_path) {
  if (lhs == rhs) return true;
  const ConfigLoadResult left = LoadConfigFromString(lhs, source_path);
  const ConfigLoadResult right = LoadConfigFromString(rhs, source_path);
  if (!left.config.has_value() || !right.config.has_value()) return false;
  return ConfigToJson(*left.config) == ConfigToJson(*right.config);
}

} // namespace

ConfigEditorModel::ConfigEditorModel(Config config)
    : config_(std::move(config)), baseline_json_(ToJson()) {}

std::vector<ConfigEditorModel::Row> ConfigEditorModel::rows() const {
  std::vector<Row> rows;
  rows.reserve(config_.active.targets.size());
  for (const auto& target : config_.active.targets) {
    Row row;
    row.slide = target.slide.value_or(0);
    row.html = target.html;
    row.label = target.label;
    row.stage_name = target.stage_name;
    row.assets_mode = target.assets_set ? target.assets.mode
                                        : config_.active.stage.mode;
    row.enabled = target.enabled;
    rows.push_back(std::move(row));
  }
  return rows;
}

bool ConfigEditorModel::HasProfiles() const {
  return !config_.profiles.empty() || !config_.active_profile.empty();
}

bool ConfigEditorModel::SetSlide(std::size_t index, int slide,
                                 std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  if (slide < 1) {
    if (error != nullptr) *error = "slide must be >= 1";
    return false;
  }
  RecordHistory();
  config_.active.targets[index].slide = slide;
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::SetHtml(std::size_t index, std::string html,
                                std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  if (html.empty()) {
    if (error != nullptr) *error = "html must not be empty";
    return false;
  }
  RecordHistory();
  config_.active.targets[index].html = std::move(html);
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::SetLabel(std::size_t index, std::string label,
                                 std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  RecordHistory();
  config_.active.targets[index].label = std::move(label);
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::SetEnabled(std::size_t index, bool enabled,
                                   std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  RecordHistory();
  config_.active.targets[index].enabled = enabled;
  dirty_ = true;
  return true;
}

std::vector<DisplayRow> ConfigEditorModel::DisplayRows(
    const DeckInfo* deck, ResolveOptions options) const {
  const ResolveResult resolved = ResolveTargets(config_, deck, options);
  std::vector<DisplayRow> rows;
  const auto base = this->rows();
  rows.reserve(base.size());
  for (std::size_t index = 0; index < base.size(); ++index) {
    DisplayRow row;
    row.index = index;
    row.slide = base[index].slide;
    // A match target owns no literal slide, so its row must show the slide the
    // resolver selected; displaying 0 made the pane rewrite it as slide 1.
    if (config_.active.targets[index].match.has_value()) {
      const int ordinal = static_cast<int>(index) + 1;
      for (const auto& resolved_target : resolved.targets) {
        if (resolved_target.ordinal == ordinal) {
          row.slide = resolved_target.slide;
          break;
        }
      }
    }
    row.label = base[index].label;
    row.html = base[index].html;
    row.assets_mode = base[index].assets_mode;
    row.assets_explicit = config_.active.targets[index].assets_set;
    row.enabled = base[index].enabled;
    const std::string prefix = "targets[" + std::to_string(index) + "]";
    for (const auto& issue : resolved.issues) {
      if (issue.severity != IssueSeverity::Error) continue;
      if (issue.path.rfind(prefix, 0) != 0) continue;
      row.issue = issue.message;
      break;
    }
    if (!row.issue.empty()) {
      row.state = "error";
    } else {
      row.state = base[index].enabled ? "resolved" : "disabled";
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string ConfigStatusText(const ConfigLoadResult& loaded,
                             const std::vector<DisplayRow>& rows,
                             const std::filesystem::path& config_path) {
  if (!loaded.config.has_value()) {
    std::string status = "cannot load " + Utf8Path(config_path);
    if (!loaded.issues.empty()) {
      status += ": " + loaded.issues.front().message;
    }
    return status;
  }
  std::string status = std::to_string(rows.size()) + " target(s)";
  for (const auto& row : rows) {
    if (row.state != "error") continue;
    status += " - " + row.issue;
    break;
  }
  return status;
}

bool ConfigEditorModel::ApplyEdit(std::size_t index, const TargetEdit& edit,
                                  std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  TargetConfig& target = config_.active.targets[index];
  // A match target is located by title or name, not by a literal slide. Keep
  // that locator intact and ignore the pane's (possibly unresolved) slide text;
  // a slide-number target still requires a valid number.
  const bool match_target = target.match.has_value();
  if (!match_target && edit.slide < 1) {
    if (error != nullptr) *error = "slide must be >= 1";
    return false;
  }
  if (edit.html.empty()) {
    if (error != nullptr) *error = "html must not be empty";
    return false;
  }
  RecordHistory();
  if (!match_target) target.slide = edit.slide;
  target.label = edit.label;
  target.html = edit.html;
  target.enabled = edit.enabled;
  if (edit.assets_mode == "inherit") {
    // The pane's explicit "use the stage mode" choice drops the target's own
    // block, so DisplayRows reports the effective stage mode again.
    target.assets_set = false;
  } else if (!edit.assets_mode.empty()) {
    // An explicit mode makes the target own its assets block; an empty mode
    // keeps whatever the target had.
    target.assets.mode = edit.assets_mode;
    target.assets_set = true;
  }
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::AddTarget(int slide, std::string html,
                                  std::string* error) {
  if (slide < 1) {
    if (error != nullptr) *error = "slide must be >= 1";
    return false;
  }
  if (html.empty()) {
    if (error != nullptr) *error = "html must not be empty";
    return false;
  }
  RecordHistory();
  TargetConfig target;
  target.slide = slide;
  target.html = std::move(html);
  target.trigger = config_.defaults.trigger;
  config_.active.targets.push_back(std::move(target));
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::ImportTargets(const std::vector<WizardPair>& pairs,
                                      std::string* error) {
  if (pairs.empty()) {
    if (error != nullptr) *error = "no pairs to import";
    return false;
  }
  // Validate the whole batch before touching the model: a rejected import must
  // leave the config, the history, and dirty() exactly as they were.
  std::vector<int> imported_slides;
  imported_slides.reserve(pairs.size());
  for (const WizardPair& pair : pairs) {
    if (pair.slide < 1) {
      if (error != nullptr) *error = "slide must be >= 1";
      return false;
    }
    if (pair.html.empty()) {
      if (error != nullptr) *error = "html must not be empty";
      return false;
    }
    for (const int slide : imported_slides) {
      if (slide == pair.slide) {
        if (error != nullptr) {
          *error = "slide " + std::to_string(pair.slide) +
                   " appears twice in the import";
        }
        return false;
      }
    }
    for (const TargetConfig& target : config_.active.targets) {
      if (target.slide.has_value() && *target.slide == pair.slide) {
        if (error != nullptr) {
          *error = "slide " + std::to_string(pair.slide) +
                   " is already mapped by an existing target";
        }
        return false;
      }
    }
    imported_slides.push_back(pair.slide);
  }
  // The whole batch is one undoable command: one pre-edit snapshot covers every
  // target appended below.
  RecordHistory();
  for (const WizardPair& pair : pairs) {
    TargetConfig target;
    target.slide = pair.slide;
    target.html = pair.html;
    target.label = pair.title;
    target.trigger = config_.defaults.trigger;
    target.enabled = true;
    config_.active.targets.push_back(std::move(target));
  }
  dirty_ = true;
  if (error != nullptr) error->clear();
  return true;
}

bool ConfigEditorModel::RemoveTarget(std::size_t index, std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  RecordHistory();
  config_.active.targets.erase(config_.active.targets.begin() +
                               static_cast<std::ptrdiff_t>(index));
  dirty_ = true;
  return true;
}

std::vector<ConfigIssue> ConfigEditorModel::Validate(
    const DeckInfo* deck, ResolveOptions options) const {
  const ResolveResult resolved = ResolveTargets(config_, deck, options);
  return resolved.issues;
}

bool ConfigEditorModel::HasBlockingError(const DeckInfo* deck,
                                         ResolveOptions options) const {
  // The gate mirrors Validate(): any resolver error blocks a save, whether it
  // belongs to a target row or to the config as a whole (a missing or empty
  // deck). A row is in the "error" state exactly when its own target has an
  // Error issue, so scanning every error with one resolver call covers the row
  // cases too and cannot disagree with the STATE column.
  const ResolveResult resolved = ResolveTargets(config_, deck, options);
  for (const auto& issue : resolved.issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

std::string ConfigEditorModel::ToJson() const { return ConfigToJson(config_); }

void ConfigEditorModel::RecordHistory() {
  undo_stack_.push_back(ToJson());
  // Drop the oldest snapshots once the cap is exceeded (keep the newest).
  if (undo_stack_.size() > kHistoryLimit) undo_stack_.erase(undo_stack_.begin());
  // A new edit branches away from any undone future.
  redo_stack_.clear();
}

bool ConfigEditorModel::RestoreSnapshot(const std::string& json) {
  // The snapshot came from ConfigToJson, so the production parser must accept
  // it; parsing (rather than keeping a Config copy) keeps the undo path honest
  // about what the loader will produce from the same text.
  const ConfigLoadResult loaded =
      LoadConfigFromString(json, config_.source_path);
  if (!loaded.config.has_value()) return false;
  Config restored = *loaded.config;
  // ConfigToJson writes only the effective profile. Keep the profile map, the
  // selector, and the legacy block so an undo can never turn a read-only
  // multi-profile config into a saveable-looking single-profile one (which
  // would drop the other profiles on the next save).
  restored.profiles = config_.profiles;
  restored.active_profile = config_.active_profile;
  restored.legacy = config_.legacy;
  config_ = std::move(restored);
  return true;
}

bool ConfigEditorModel::Undo() {
  if (undo_stack_.empty()) return false;
  const std::string current = ToJson();
  const std::string snapshot = undo_stack_.back();
  if (!RestoreSnapshot(snapshot)) return false;
  undo_stack_.pop_back();
  redo_stack_.push_back(current);
  dirty_ = !JsonIsEquivalent(ToJson(), baseline_json_, config_.source_path);
  return true;
}

bool ConfigEditorModel::Redo() {
  if (redo_stack_.empty()) return false;
  const std::string current = ToJson();
  const std::string snapshot = redo_stack_.back();
  if (!RestoreSnapshot(snapshot)) return false;
  redo_stack_.pop_back();
  undo_stack_.push_back(current);
  if (undo_stack_.size() > kHistoryLimit) undo_stack_.erase(undo_stack_.begin());
  dirty_ = !JsonIsEquivalent(ToJson(), baseline_json_, config_.source_path);
  return true;
}

void ConfigEditorModel::ResetHistory() {
  undo_stack_.clear();
  redo_stack_.clear();
  baseline_json_ = ToJson();
  dirty_ = false;
}

bool ConfigEditorModel::Save(const std::filesystem::path& path,
                             std::string* error) {
  const auto temp = path.string() + ".tmp";
  {
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error != nullptr) *error = "cannot write " + temp;
      return false;
    }
    const std::string json = ToJson();
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.close();
    if (!output.good()) {
      if (error != nullptr) *error = "cannot finish " + temp;
      return false;
    }
  }
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  std::error_code rename_error;
  std::filesystem::rename(temp, path, rename_error);
  if (rename_error) {
    if (error != nullptr) {
      *error = "cannot replace " + path.string() + ": " +
               rename_error.message();
    }
    return false;
  }
  // The saved config is the new clean baseline: nothing before it is undoable.
  ResetHistory();
  return true;
}

bool ConfigEditorModel::SaveWithBackup(const std::filesystem::path& path,
                                       std::string* error) {
  const auto temp = path.string() + ".tmp";
  {
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error != nullptr) *error = "cannot write " + temp;
      return false;
    }
    const std::string json = ToJson();
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.close();
    if (!output.good()) {
      std::error_code cleanup_error;
      std::filesystem::remove(temp, cleanup_error);
      if (error != nullptr) *error = "cannot finish " + temp;
      return false;
    }
  }
  std::error_code exists_error;
  if (std::filesystem::is_regular_file(path, exists_error)) {
    std::error_code backup_error;
    std::filesystem::copy_file(
        path, path.string() + ".bak",
        std::filesystem::copy_options::overwrite_existing, backup_error);
    if (backup_error) {
      std::error_code cleanup_error;
      std::filesystem::remove(temp, cleanup_error);
      if (error != nullptr) {
        *error = "cannot write " + path.string() + ".bak: " +
                 backup_error.message();
      }
      return false;
    }
  }
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  std::error_code rename_error;
  std::filesystem::rename(temp, path, rename_error);
  if (rename_error) {
    std::error_code cleanup_error;
    std::filesystem::remove(temp, cleanup_error);
    if (error != nullptr) {
      *error = "cannot replace " + path.string() + ": " +
               rename_error.message();
    }
    return false;
  }
  // The saved config is the new clean baseline: nothing before it is undoable.
  ResetHistory();
  return true;
}

bool ConfigEditorModel::ReloadFromDisk(const std::filesystem::path& path,
                                       std::string* error) {
  const ConfigLoadResult loaded = LoadConfigFile(path);
  if (!loaded.config.has_value()) {
    if (error != nullptr) {
      *error = "cannot load " + Utf8Path(path);
      if (!loaded.issues.empty()) {
        *error += ": " + loaded.issues.front().message;
      }
    }
    return false;
  }
  config_ = *loaded.config;
  // The file on disk is the new clean baseline: an undo must never cross an
  // external reload and resurrect content that is no longer on disk.
  ResetHistory();
  if (error != nullptr) error->clear();
  return true;
}

} // namespace htmlppt
