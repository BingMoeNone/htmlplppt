#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/resolve.h"

namespace htmlppt {

struct StageOutcome {
  // The path Edge should open: the source for direct mode, the staged copy
  // otherwise.
  std::filesystem::path html_path;
  // Bytes copied for this target; zero for direct mode.
  std::uint64_t copied_bytes = 0;
  // False when staging was skipped or failed, so the caller can fall back to
  // the source path and journal the message.
  bool staged = false;
  std::string error;
};

// Stages one target's HTML for Edge. "direct" leaves the source path alone;
// "file" copies only the HTML into stage_dir; "tree" copies the HTML and the
// whole asset root tree into stage_dir. Destination names use the target's
// ASCII stage_name, so a CJK source file still produces a predictable URL.
// max_bytes (0 = unlimited) caps the total copy size and fails with a message
// instead of silently truncating.
StageOutcome StageTargetHtml(const ResolvedTarget& target,
                             const std::filesystem::path& stage_dir,
                             std::int64_t max_bytes);

} // namespace htmlppt
