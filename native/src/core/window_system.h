#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace htmlppt {

using WindowId = std::uintptr_t;

// How hard the focus layer should try to become foreground (config
// focus.strategy). Auto is the full multi-stage chain; the other two restrict it
// to one technique, which is what the spec's strategy switch is for.
enum class FocusStrategy { Auto, AttachAlt, Switch };

struct WindowRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
  long long area() const {
    return static_cast<long long>(width()) * static_cast<long long>(height());
  }
};

struct WindowInfo {
  WindowId id = 0;
  std::uint32_t pid = 0;
  std::string class_name;
  std::string title;
  WindowRect rect;
  bool visible = false;
  bool topmost = false;
  bool ws_popup = false;
  bool has_min_box = true;
  bool has_max_box = true;
};

struct ScreenMetrics {
  int width = 0;
  int height = 0;
};

class WindowSystem {
 public:
  virtual ~WindowSystem() = default;

  virtual std::vector<WindowInfo> Enumerate() = 0;
  virtual WindowId Foreground() = 0;
  virtual bool IsForeground(WindowId id) = 0;
  virtual bool Activate(WindowId id) = 0;
  // Strategy-aware activation. Platforms that cannot vary the technique keep the
  // plain Activate() behaviour.
  virtual bool ActivateWith(WindowId id, FocusStrategy strategy) {
    (void)strategy;
    return Activate(id);
  }
  virtual bool IsWindow(WindowId id) = 0;
};

} // namespace htmlppt
