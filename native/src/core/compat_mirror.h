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
// where it becomes the agent-arm profile; every other row ignores it.
// `flag_dir` is the compat base directory (the folder the flag files live in);
// it is read in the same row to spell the two paths the persistent focus agent
// writes, with Windows separators. Both strings are only read on a transition
// into Live.
//
// Covered transitions (each row is one flag file, in this order; the source
// state may be any state except the destination itself):
//
//   -> CoverIn      cover-fade.flag   (empty)
//   -> Live         cover-ready.flag  (empty)
//                   agent-arm.flag    ("<edge_profile>|<flag_dir>\edge-closed.flag|<flag_dir>\cover-ready.flag")
//   -> SafeMode     cover-hide.flag   (empty)
//   -> Stopping     cover-stop.flag   (empty)
//                   agent-stop.flag   (empty)
//   -> Exited       cover-stop.flag   (empty)
//                   agent-stop.flag   (empty)
//   Live -> Monitoring | Returning | ReturnCoverOut
//                   edge-closed.flag  (empty)
//
// The order matters: a reader that sees the second row of a pair can assume the
// first one is already on disk. The Exited and return-path rows are the ones a
// real WorkerRuntime actually publishes: its cleanup collapses Stopping into
// Exited inside one Step, and its return path runs through Returning and
// ReturnCoverOut before Monitoring. The literal Stopping and Live -> Monitoring
// rows are kept for drivers that publish those transitions directly.
std::vector<FlagWrite> FlagsForStateTransition(State from, State to,
                                               std::string_view edge_profile,
                                               std::string_view flag_dir);

} // namespace htmlppt
