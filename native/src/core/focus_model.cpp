#include "core/focus_model.h"

namespace htmlppt {
namespace {

bool Contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

} // namespace

bool IsReturnTitle(std::string_view title) {
  return title.find(kReturnTitleMarker) != std::string_view::npos;
}

std::optional<WindowId> SelectEdgeWindow(const std::vector<WindowInfo>& windows,
                                         std::uint32_t edge_pid) {
  std::optional<WindowId> best;
  long long best_area = -1;
  for (const auto& window : windows) {
    if (window.pid != edge_pid || !window.visible) continue;
    const long long area = window.rect.area();
    if (area > best_area) {
      best_area = area;
      best = window.id;
    }
  }
  return best;
}

int ScoreWpsWindow(const WindowInfo& window, ScreenMetrics screen) {
  if (!window.visible) return 0;
  int score = 0;
  if (window.class_name == "screenClass" ||
      window.class_name == "SlideShowClass" ||
      window.class_name == "Widget") {
    score += 2;
  }
  // UTF-8 for the WPS slideshow title word.
  static constexpr char kWpsTitleWord[] =
      "\xE5\xB9\xBB" "\xE7\x81\xAF" "\xE7\x89\x87";
  if (Contains(window.title, kWpsTitleWord)) score += 3;
  if (Contains(window.title, "Slide Show") ||
      Contains(window.title, "Slideshow")) {
    score += 3;
  }
  if (window.ws_popup) score += 2;
  if (!window.has_min_box && !window.has_max_box) score += 2;
  if (screen.width > 0 && screen.height > 0) {
    const int min_width = static_cast<int>(screen.width * 0.9);
    const int min_height = static_cast<int>(screen.height * 0.9);
    if (window.rect.width() < min_width || window.rect.height() < min_height) {
      return 0;
    }
    score += 1;
  }
  return score;
}

std::optional<WindowId> SelectWpsShowWindow(
    const std::vector<WindowInfo>& windows,
    const std::vector<std::uint32_t>& wps_pids, ScreenMetrics screen) {
  std::optional<WindowId> best;
  int best_score = 0;
  long long best_area = -1;
  for (const auto& window : windows) {
    if (!window.visible) continue;
    bool pid_match = false;
    for (const auto pid : wps_pids) {
      if (window.pid == pid) {
        pid_match = true;
        break;
      }
    }
    if (!pid_match) continue;
    const int score = ScoreWpsWindow(window, screen);
    // Require at least 4 points; a maximized editor scores 3.
    if (score < 4) continue;
    const long long area = window.rect.area();
    const bool better =
        !best.has_value() || score > best_score ||
        (score == best_score &&
         (area > best_area || (area == best_area && window.id < *best)));
    if (better) {
      best_score = score;
      best_area = area;
      best = window.id;
    }
  }
  return best;
}

} // namespace htmlppt
