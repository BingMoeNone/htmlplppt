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
    } else if (token == "sleep-resume") {
      faults.sleep_resume = true;
    } else if (token == "multi-monitor") {
      faults.multi_monitor = true;
    } else if (token == "cleanup-slow") {
      faults.cleanup_slow = true;
    } else if (token == "start-fail") {
      faults.start_fail = true;
    }
  }
  return faults;
}

bool HasAnyFault(const Faults& faults) {
  return faults.com_hang || faults.com_462 || faults.edge_slow || faults.supervisor_loss ||
         faults.cover_hang || faults.focus_deny || faults.sleep_resume ||
         faults.multi_monitor || faults.cleanup_slow || faults.start_fail;
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
  if (faults.sleep_resume) add("sleep-resume");
  if (faults.multi_monitor) add("multi-monitor");
  if (faults.cleanup_slow) add("cleanup-slow");
  if (faults.start_fail) add("start-fail");
  return text;
}

} // namespace htmlppt
