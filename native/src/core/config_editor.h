#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/deck_info.h"
#include "core/resolve.h"

namespace htmlppt {

// The editable fields of one target, as the UI dialog collects them. An empty
// assets_mode leaves the target's own assets block (or its absence) untouched.
struct TargetEdit {
  int slide = 1;
  std::string label;
  std::string html;
  bool enabled = true;
  std::string assets_mode;
};

// Testable half of the config UI (design spec 9.5): the Win32 view is a thin
// skin over this model. It edits the *effective* profile (Config::active), which
// is what ConfigToJson writes, so Save() produces a runnable config.
class ConfigEditorModel {
 public:
  struct Row {
    int slide = 0;
    std::string html;
    std::string label;
    std::string stage_name;
    std::string assets_mode;
    bool enabled = true;
  };

  explicit ConfigEditorModel(Config config);

  const Config& config() const { return config_; }
  // One row per effective target, in config order.
  std::vector<Row> rows() const;
  std::size_t target_count() const { return config_.active.targets.size(); }
  // True once an edit changed the model and Save() has not been called.
  bool dirty() const { return dirty_; }

  bool SetSlide(std::size_t index, int slide, std::string* error);
  bool SetHtml(std::size_t index, std::string html, std::string* error);
  bool SetLabel(std::size_t index, std::string label, std::string* error);
  bool SetEnabled(std::size_t index, bool enabled, std::string* error);
  // One dialog result -> one dirty transition and one error path.
  bool ApplyEdit(std::size_t index, const TargetEdit& edit, std::string* error);
  bool AddTarget(int slide, std::string html, std::string* error);
  bool RemoveTarget(std::size_t index, std::string* error);

  // Full validation through the same resolver a run uses. Errors name the
  // offending target, so the view can point at the row.
  std::vector<ConfigIssue> Validate(const DeckInfo* deck,
                                    ResolveOptions options) const;

  std::string ToJson() const;

  // Atomic save: write a sibling temp file, then replace the target. A failed
  // write leaves the previous file untouched.
  bool Save(const std::filesystem::path& path, std::string* error);
  // Same, plus a <path>.bak copy of the previous file when one existed. A
  // failed write still leaves the previous file untouched and removes the temp.
  bool SaveWithBackup(const std::filesystem::path& path, std::string* error);

 private:
  Config config_;
  bool dirty_ = false;
};

} // namespace htmlppt
