#include "app/config_ui.h"

#include <iostream>

#ifdef _WIN32

#include "platform/win32/config_window.h"

namespace htmlppt {

int RunConfigUi(const std::filesystem::path& config_path,
                const std::filesystem::path& deck_info_path) {
  // Task 5 loads the config file and the optional deck info; this task owns the
  // window lifecycle only.
  (void)deck_info_path;
  ConfigWindow window;
  ConfigWindowOptions options;
  options.title = L"htmlppt config - " + config_path.wstring();
  if (!window.Create(options)) {
    std::cerr << "error: cannot create the config window\n";
    return 1;
  }
  return window.RunMessageLoop();
}

} // namespace htmlppt

#else

namespace htmlppt {

int RunConfigUi(const std::filesystem::path&, const std::filesystem::path&) {
  std::cerr << "error: --config-ui requires Windows\n";
  return 2;
}

} // namespace htmlppt

#endif // _WIN32
