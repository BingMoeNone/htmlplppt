#include "core/config_editor.h"

#include <fstream>
#include <utility>

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

} // namespace

ConfigEditorModel::ConfigEditorModel(Config config)
    : config_(std::move(config)) {}

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

bool ConfigEditorModel::SetSlide(std::size_t index, int slide,
                                 std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  if (slide < 1) {
    if (error != nullptr) *error = "slide must be >= 1";
    return false;
  }
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
  config_.active.targets[index].html = std::move(html);
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::SetLabel(std::size_t index, std::string label,
                                 std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  config_.active.targets[index].label = std::move(label);
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::SetEnabled(std::size_t index, bool enabled,
                                   std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
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
    row.label = base[index].label;
    row.html = base[index].html;
    row.assets_mode = base[index].assets_mode;
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

bool ConfigEditorModel::ApplyEdit(std::size_t index, const TargetEdit& edit,
                                  std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
  if (edit.slide < 1) {
    if (error != nullptr) *error = "slide must be >= 1";
    return false;
  }
  if (edit.html.empty()) {
    if (error != nullptr) *error = "html must not be empty";
    return false;
  }
  TargetConfig& target = config_.active.targets[index];
  target.slide = edit.slide;
  target.label = edit.label;
  target.html = edit.html;
  target.enabled = edit.enabled;
  if (!edit.assets_mode.empty()) {
    // An explicit mode makes the target own its assets block; otherwise the
    // global stage mode keeps applying.
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
  TargetConfig target;
  target.slide = slide;
  target.html = std::move(html);
  target.trigger = config_.defaults.trigger;
  config_.active.targets.push_back(std::move(target));
  dirty_ = true;
  return true;
}

bool ConfigEditorModel::RemoveTarget(std::size_t index, std::string* error) {
  if (!CheckIndex(index, target_count(), error)) return false;
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

std::string ConfigEditorModel::ToJson() const { return ConfigToJson(config_); }

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
  dirty_ = false;
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
  dirty_ = false;
  return true;
}

} // namespace htmlppt
