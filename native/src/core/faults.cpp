#include "core/faults.h"

#include <string>
#include <vector>

namespace htmlppt {
namespace {

void SplitTokens(std::string_view text, std::vector<std::string>* out) {
  std::string current;
  for (const char c : text) {
    if (c == ',' || c == ' ' || c == '\t' || c == ';') {
      if (!current.empty()) out->push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) out->push_back(current);
}

} // namespace

Faults ParseFaults(std::string_view text) {
  Faults faults;
  std::vector<std::string> tokens;
  SplitTokens(text, &tokens);
  for (const auto& token : tokens) {
    if (token == "com-hang") {
      faults.com_hang = true;
    } else if (token == "com-462") {
      faults.com_462 = true;
    } else if (token == "edge-slow") {
      faults.edge_slow = true;
    } else if (token == "cover-hang") {
      faults.cover_hang = true;
    } else if (token == "focus-deny") {
      faults.focus_deny = true;
    } else if (token == "supervisor-loss") {
      faults.supervisor_loss = true;
    }
  }
  return faults;
}

bool HasAnyFault(const Faults& faults) {
  return faults.com_hang || faults.com_462 || faults.edge_slow || faults.supervisor_loss ||
         faults.cover_hang || faults.focus_deny;
}

std::string FaultsToString(const Faults& faults) {
  std::string text;
  const auto add = [&text](const char* name) {
    if (!text.empty()) text += ",";
    text += name;
  };
  if (faults.com_hang) add("com-hang");
  if (faults.com_462) add("com-462");
  if (faults.edge_slow) add("edge-slow");
  if (faults.cover_hang) add("cover-hang");
  if (faults.focus_deny) add("focus-deny");
  if (faults.supervisor_loss) add("supervisor-loss");
  return text;
}

} // namespace htmlppt
