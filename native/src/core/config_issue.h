#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/config.h"

namespace htmlppt {

inline void AddIssue(std::vector<ConfigIssue>& issues, IssueSeverity severity,
                     std::string path, std::string message) {
  issues.push_back({severity, std::move(path), std::move(message)});
}

inline void AddError(std::vector<ConfigIssue>& issues, std::string path,
                     std::string message) {
  AddIssue(issues, IssueSeverity::Error, std::move(path), std::move(message));
}

inline void AddWarning(std::vector<ConfigIssue>& issues, std::string path,
                       std::string message) {
  AddIssue(issues, IssueSeverity::Warning, std::move(path), std::move(message));
}

inline bool HasErrorIssue(const std::vector<ConfigIssue>& issues) {
  for (const auto& issue : issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

} // namespace htmlppt
