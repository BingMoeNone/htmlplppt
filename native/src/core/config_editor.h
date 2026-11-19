#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/deck_info.h"
#include "core/mapping_wizard.h"
#include "core/resolve.h"

namespace htmlppt {

// The editable fields of one target, as the UI dialog collects them. The
// assets mode has three states: empty leaves the target's own assets block (or
// its absence) untouched, "inherit" clears that block so the stage mode applies
// again, and any other value makes the target own an explicit block with that
// mode.
struct TargetEdit {
  int slide = 1;
  std::string label;
  std::string html;
  bool enabled = true;
  std::string assets_mode;
};

// One table row for the UI: the editable fields plus the validation state the
// window renders in its STATE column.
struct DisplayRow {
  std::size_t index = 0;
  int slide = 0;
  std::string label;
  std::string html;
  std::string assets_mode;
  // True when the target carries its own assets block; false means the stage
  // mode applies and the UI presents the mode as inherited.
  bool assets_explicit = false;
  // The target's own switch, independent of the validation state below.
  bool enabled = true;
  // "resolved", "disabled", or "error".
  std::string state;
  // First error for this target, empty when the row is clean.
  std::string issue;
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
  // Same, plus the per-target validation state from the real resolver.
  std::vector<DisplayRow> DisplayRows(const DeckInfo* deck,
                                      ResolveOptions options) const;
  std::size_t target_count() const { return config_.active.targets.size(); }
  // True when the source config is profile-based. ConfigToJson() writes only the
  // effective profile, so saving such a config would drop `profiles` and
  // `activeProfile`; the UI must refuse to save instead.
  bool HasProfiles() const;
  // True once an edit changed the model and Save() has not been called.
  bool dirty() const { return dirty_; }

  bool SetSlide(std::size_t index, int slide, std::string* error);
  bool SetHtml(std::size_t index, std::string html, std::string* error);
  bool SetLabel(std::size_t index, std::string label, std::string* error);
  bool SetEnabled(std::size_t index, bool enabled, std::string* error);
  // One dialog result -> one dirty transition and one error path.
  bool ApplyEdit(std::size_t index, const TargetEdit& edit, std::string* error);
  bool AddTarget(int slide, std::string html, std::string* error);
  // Appends one target per pair as a single undoable edit: slide, html, and
  // label (the slide title) come from the pair, and every target is enabled.
  // The batch is rejected as a whole -- leaving the model untouched -- when it
  // is empty, names an invalid slide or an empty html, repeats a slide within
  // itself, or reuses an existing target's slide. Returns false with `error`
  // set on rejection.
  bool ImportTargets(const std::vector<WizardPair>& pairs, std::string* error,
                     int* skipped = nullptr);
  bool RemoveTarget(std::size_t index, std::string* error);

  // Full validation through the same resolver a run uses. Errors name the
  // offending target, so the view can point at the row.
  std::vector<ConfigIssue> Validate(const DeckInfo* deck,
                                    ResolveOptions options) const;

  // True when validation reports any error, per-target or global (an empty or
  // missing deck, say), i.e. saving now would persist a config that cannot run.
  // A disabled target is not an error.
  bool HasBlockingError(const DeckInfo* deck, ResolveOptions options) const;

  std::string ToJson() const;

  // Undo/redo over bounded JSON snapshots: every successful mutation records
  // the pre-edit state, a new mutation discards the redo path, and the undo
  // stack keeps at most the newest 50 snapshots. Restoring parses the snapshot
  // through the production parser, so an undo cannot invent a config the loader
  // would reject. dirty() is then recomputed against the baseline from the last
  // load or save instead of being forced true, so undoing back to the saved
  // state is clean again. Undo/Redo past the end return false and change
  // nothing; a load (construction) and a successful save both reset the history.
  bool Undo();
  bool Redo();
  bool CanUndo() const { return !undo_stack_.empty(); }
  bool CanRedo() const { return !redo_stack_.empty(); }

  // Atomic save: write a sibling temp file, then replace the target. A failed
  // write leaves the previous file untouched.
  bool Save(const std::filesystem::path& path, std::string* error);
  // Same, plus a <path>.bak copy of the previous file when one existed. A
  // failed write still leaves the previous file untouched and removes the temp.
  bool SaveWithBackup(const std::filesystem::path& path, std::string* error);

  // Replaces the model with `path`, read through the production loader, as a
  // clean baseline: the undo/redo history never crosses an external reload. A
  // file that does not load leaves the model untouched and reports the first
  // issue; returns false.
  bool ReloadFromDisk(const std::filesystem::path& path,
                      std::string* error = nullptr);

 private:
  // The undo stack is capped so a long editing session stays bounded.
  static constexpr std::size_t kHistoryLimit = 50;

  // Records the state before a successful mutation and clears the redo path.
  void RecordHistory();
  // Parses one snapshot back into config_, keeping the profile state the
  // effective-profile serializer does not carry and the original source path.
  bool RestoreSnapshot(const std::string& json);
  // Drops both stacks and makes the current state the dirty() baseline.
  void ResetHistory();

  Config config_;
  bool dirty_ = false;
  // The JSON of the last loaded or saved state: after an undo/redo dirty() is
  // recomputed by comparing against this, not simply set.
  std::string baseline_json_;
  std::vector<std::string> undo_stack_;
  std::vector<std::string> redo_stack_;
};

// One-line status for the editor window. For a file that did not load it is
// "cannot load <path>: <first issue message>"; otherwise it is the row count
// ("<N> target(s)") plus " - <issue>" for the first row in the error state.
std::string ConfigStatusText(const ConfigLoadResult& loaded,
                             const std::vector<DisplayRow>& rows,
                             const std::filesystem::path& config_path);

} // namespace htmlppt
