#pragma once

#ifdef _WIN32

#include <vector>

#include "core/window_system.h"

namespace htmlppt {

ScreenMetrics PrimaryScreenMetrics();

class Win32WindowSystem final : public WindowSystem {
 public:
  std::vector<WindowInfo> Enumerate() override;
  WindowId Foreground() override;
  bool IsForeground(WindowId id) override;
  bool Activate(WindowId id) override;
  bool IsWindow(WindowId id) override;
};

} // namespace htmlppt

#endif // _WIN32
