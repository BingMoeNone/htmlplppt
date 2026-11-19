#include "platform/win32/screen_win32.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace htmlppt {
namespace {

struct MonitorPick {
  RECT rect{};
  bool primary = false;
};

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* monitors = reinterpret_cast<std::vector<MonitorPick>*>(data);
  MONITORINFO info{};
  info.cbSize = sizeof(MONITORINFO);
  if (GetMonitorInfoW(monitor, &info) != 0) {
    MonitorPick pick;
    pick.rect = info.rcMonitor;
    pick.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitors->push_back(pick);
  }
  return TRUE;
}

std::vector<MonitorPick> EnumerateMonitors() {
  std::vector<MonitorPick> monitors;
  EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                      reinterpret_cast<LPARAM>(&monitors));
  return monitors;
}

ScreenMetrics MetricsOf(const RECT& rect) {
  ScreenMetrics metrics;
  metrics.width = rect.right - rect.left;
  metrics.height = rect.bottom - rect.top;
  return metrics;
}

} // namespace

ScreenSelection SelectScreenMetrics(std::string_view selector) {
  ScreenSelection selection;
  const auto monitors = EnumerateMonitors();
  const auto primary_metrics = [] {
    ScreenMetrics metrics;
    metrics.width = GetSystemMetrics(SM_CXSCREEN);
    metrics.height = GetSystemMetrics(SM_CYSCREEN);
    return metrics;
  };

  if (monitors.empty()) {
    selection.metrics = primary_metrics();
    if (!selector.empty() && selector != "primary") {
      selection.error = "no monitor topology available; using the primary";
    }
    return selection;
  }

  const auto pick_primary = [&monitors]() {
    for (const auto& monitor : monitors) {
      if (monitor.primary) return monitor;
    }
    return monitors.front();
  };

  if (selector.empty() || selector == "primary") {
    selection.metrics = MetricsOf(pick_primary().rect);
    return selection;
  }
  if (selector == "secondary") {
    for (const auto& monitor : monitors) {
      if (!monitor.primary) {
        selection.metrics = MetricsOf(monitor.rect);
        return selection;
      }
    }
    selection.metrics = MetricsOf(pick_primary().rect);
    selection.error = "no secondary monitor; using the primary";
    return selection;
  }

  // A 1-based index, in the order EnumDisplayMonitors reports them.
  std::size_t index = 0;
  bool numeric = !selector.empty();
  for (const char c : selector) {
    if (c < '0' || c > '9') {
      numeric = false;
      break;
    }
    index = index * 10 + static_cast<std::size_t>(c - '0');
  }
  if (numeric && index >= 1 && index <= monitors.size()) {
    selection.metrics = MetricsOf(monitors[index - 1].rect);
    return selection;
  }
  selection.metrics = MetricsOf(pick_primary().rect);
  selection.error = "monitor selector out of range: " + std::string(selector);
  return selection;
}

} // namespace htmlppt
