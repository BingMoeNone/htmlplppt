#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace htmlppt {

// One legacy flag file the mirror writes for a state transition. `payload` is
// the exact file content; an empty payload still creates an empty flag file.
struct FlagWrite {
  std::string name;
  std::string payload;
};

// Maps one state transition to the legacy flag files the worker must write, in
// write order. Identical states and transitions the legacy protocol does not
// signal return an empty vector. `edge_profile` is read only when entering Live,
// where it becomes the agent-arm payload; every other row ignores it.
//
// Covered transitions (each row is one flag file, in this order; the source
// state may be any state except the destination itself):
//
//   -> CoverIn      cover-fade.flag   (empty)
//   -> Live         cover-ready.flag  (empty)
//                   agent-arm.flag    ("<edge_profile>|exit=0|ready=1")
//   -> SafeMode     cover-hide.flag   (empty)
//   -> Stopping     cover-stop.flag   (empty)
//                   agent-stop.flag   (empty)
//   Live -> Monitoring  edge-closed.flag  (empty)
//
// The order matters: a reader that sees the second row of a pair can assume the
// first one is already on disk.
std::vector<FlagWrite> FlagsForStateTransition(State from, State to,
                                               std::string_view edge_profile);

} // namespace htmlppt
