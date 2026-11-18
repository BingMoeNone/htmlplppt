#pragma once

#include <string>
#include <vector>

namespace htmlppt {

// One slide of the deck the wizard proposes targets for.
struct WizardSlide {
  int number = 0;     // 1-based slide number
  std::string title;  // title text, may be empty
};

// One proposed slide -> HTML pairing. `included` starts true for every
// proposal; the wizard clears it when the user unchecks the row.
struct WizardPair {
  int slide = 0;
  std::string title;
  std::string html;
  bool included = true;
};

// How the wizard guesses the mapping. Both strategies are suggestions only:
// every pair is shown in the preview and the user confirms before anything is
// written to the model.
enum class PairingMode {
  // The trailing integer of the file stem names the slide: 3.html, page-03.html
  // and slide-3.html all propose slide 3.
  ByFileName,
  // The sorted HTML file list is zipped with the deck's slide order.
  ByFileOrder,
};

// Proposes one pair per matched slide, in deck order. Only *.html / *.htm
// files (case-insensitive) are considered; `html_files` may be bare names or
// paths, as in the wizard's folder scan.
//
// ByFileName: the first file (in input order) whose stem ends in the slide's
// number wins, and a file whose number has no slide is ignored. ByFileOrder:
// the HTML files are sorted lexicographically and zipped with the slides; the
// longer side's leftovers are ignored.
std::vector<WizardPair> ProposePairs(const std::vector<WizardSlide>& slides,
                                     const std::vector<std::string>& html_files,
                                     PairingMode mode);

// The longest common directory prefix of `html_files`, with '/' separators and
// no trailing separator. A file with no directory, an empty list, or files
// with no shared directory yields "". Native '\' separators are accepted, so a
// Windows folder scan's UTF-8 paths work.
std::string SuggestHtmlRoot(const std::vector<std::string>& html_files);

} // namespace htmlppt
