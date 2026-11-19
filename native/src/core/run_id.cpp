#include "core/run_id.h"

#include <cstddef>

namespace htmlppt {

bool IsValidRunId(std::string_view run_id) {
  if (run_id.empty() || run_id.size() > 128) return false;
  if (run_id == "." || run_id == "..") return false;
  for (const char c : run_id) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '-';
    if (!safe) return false;
  }
  return true;
}

} // namespace htmlppt
