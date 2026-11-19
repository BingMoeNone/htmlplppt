#include "app/deck_info_loader.h"

#include <utility>

#include "core/resolve.h"
#ifdef _WIN32
#include "core/clock.h"
#include "platform/win32/wps_host.h"
#endif

namespace htmlppt {

DeckInfoLoad LoadDeckInfo(const std::filesystem::path& deck_info_path,
                          const Config& config) {
  DeckInfoLoad result;
  // A given info file always wins: it is the deterministic path used in CI and
  // on machines without WPS. The deck path comes from the loaded config.
  if (!deck_info_path.empty()) {
    JsonDeckInfoProvider provider(deck_info_path);
    auto loaded = provider.Load(ResolveDeckPath(config));
    result.issues = std::move(loaded.issues);
    result.info = std::move(loaded.info);
    return result;
  }
#ifdef _WIN32
  // No info file: ask WPS for the live deck, exactly like --validate does.
  SteadyClock clock;
  WpsHostConfig host_config;
  host_config.launch_attempts = 1;
  WpsHost host(clock, host_config);
  auto loaded = host.Load(ResolveDeckPath(config));
  host.Release();
  result.issues = std::move(loaded.issues);
  result.info = std::move(loaded.info);
#endif
  return result;
}

} // namespace htmlppt
